/* T1: draw-call submission cost on the CPU. N tiny quads per frame (the
   GPU is nearly idle), one glDrawArrays each, with a state change per draw
   that depends on the variant:
     none, uniform, texbind (pool of 64), blend (func toggles),
     program (8 programs), fbo (switch every 50 draws),
     mix: the Part B state mix (82 % textured, 74 % blended, a uniform per
          draw for colour / CLUT, program changes, 6 RT switches per frame).
   Vertex paths: client arrays, a streaming VBO ring (one BufferSubData per
   draw), one static VBO with offsets. mix_gs_t1 / mix_gs_t2 add the
   conversion of GS PACKED vertices (48 bytes each) to GL vertices, on the
   render thread or on a producer thread that runs one frame ahead. */
#define _POSIX_C_SOURCE 200809L
#include "gl_bench.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define MAXQ 4000
#define RT_W 640
#define RT_H 448
#define RING_BYTES (4u << 20)

enum { P_CLIENT, P_STREAM, P_STATIC, P_COUNT };
static const char *pname[P_COUNT] = { "client", "stream", "static" };
enum { V_NONE, V_UNIFORM, V_TEXBIND, V_BLEND, V_PROGRAM, V_FBO, V_MIX, V_COUNT };
static const char *vname[V_COUNT] = { "none", "uniform", "texbind", "blend", "program", "fbo", "mix" };

typedef struct {
    uint8_t textured, prog, blended, rt;
    uint8_t tex;      /* pool index; 255 = keep current */
} mix_op;

static struct {
    float verts[MAXQ * 16];         /* 4 vertices x (x, y, u, v) */
    uint32_t gsverts[MAXQ * 4 * 12]; /* the same quads as GS PACKED qwords */
    GLuint prog_tex[8], prog_col;
    GLint ucol_tex[8], ucol_col;
    GLuint pool[64];
    gl_fbo fb[2];
    GLuint vbo_static, vbo_ring, vao_static, vao_ring;
    GLuint ring_off;
    mix_op ops[MAXQ];
} S;

static const char *FS_TEX =
    "uniform sampler2D u_tex;\n"
    "uniform vec4 u_color;\n"
    "in vec2 v_uv;\n"
    "out vec4 o;\n"
    "void main() { o = texture(u_tex, v_uv) * u_color * K; }\n";

static const char *FS_COL =
    "uniform vec4 u_color;\n"
    "out vec4 o;\n"
    "void main() { o = u_color; }\n";

static void setup(void)
{
    uint64_t seed = 0x71;
    for (int i = 0; i < MAXQ; i++) {
        float x = (float)(rng_next(&seed) % (RT_W - 2)), y = (float)(rng_next(&seed) % (RT_H - 2));
        for (int v = 0; v < 4; v++) {
            float px = x + (float)(v & 1) * 2, py = y + (float)(v >> 1) * 2;
            float *o = &S.verts[i * 16 + v * 4];
            o[0] = px / RT_W * 2 - 1;
            o[1] = py / RT_H * 2 - 1;
            o[2] = (float)(v & 1);
            o[3] = (float)(v >> 1);
            uint32_t *g = &S.gsverts[(i * 4 + v) * 12];
            float s = o[2], t = o[3], q = 1.0f;
            memcpy(&g[0], &s, 4);
            memcpy(&g[1], &t, 4);
            memcpy(&g[2], &q, 4);
            g[3] = 0;
            g[4] = 128, g[5] = 128, g[6] = 128, g[7] = 128;
            g[8] = (uint32_t)((px + 2048.0f - RT_W / 2) * 16.0f); /* 12.4 fixed, GS offset */
            g[9] = (uint32_t)((py + 2048.0f - RT_H / 2) * 16.0f);
            g[10] = 0x123456;
            g[11] = 0;
        }
    }
    char fs[512];
    for (int k = 0; k < 8; k++) {
        snprintf(fs, sizeof fs, "#define K %.3f\n%s", 1.0 + k * 0.01, FS_TEX);
        S.prog_tex[k] = gl_program(NULL, VS_BASIC, fs);
        glUniform1i(glGetUniformLocation(S.prog_tex[k], "u_tex"), 0);
        S.ucol_tex[k] = glGetUniformLocation(S.prog_tex[k], "u_color");
        glUniform4f(S.ucol_tex[k], 1, 1, 1, 1);
    }
    S.prog_col = gl_program(NULL, VS_BASIC, FS_COL);
    S.ucol_col = glGetUniformLocation(S.prog_col, "u_color");
    for (int i = 0; i < 64; i++)
        S.pool[i] = gl_tex_rgba8(16, 16, 0x900 + (uint64_t)i, 0, 1);
    gl_fbo_init(&S.fb[0], RT_W, RT_H, 0);
    gl_fbo_init(&S.fb[1], RT_W, RT_H, 0);

    glGenVertexArrays(1, &S.vao_static);
    glBindVertexArray(S.vao_static);
    glGenBuffers(1, &S.vbo_static);
    glBindBuffer(GL_ARRAY_BUFFER, S.vbo_static);
    glBufferData(GL_ARRAY_BUFFER, sizeof S.verts, S.verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);

    glGenVertexArrays(1, &S.vao_ring);
    glBindVertexArray(S.vao_ring);
    glGenBuffers(1, &S.vbo_ring);
    glBindBuffer(GL_ARRAY_BUFFER, S.vbo_ring);
    glBufferData(GL_ARRAY_BUFFER, RING_BYTES, NULL, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
    glBindVertexArray(0);
    gl_check("t1 setup");
}

static void teardown(void)
{
    for (int k = 0; k < 8; k++)
        glDeleteProgram(S.prog_tex[k]);
    glDeleteProgram(S.prog_col);
    glDeleteTextures(64, S.pool);
    gl_fbo_free(&S.fb[0]);
    gl_fbo_free(&S.fb[1]);
    glDeleteBuffers(1, &S.vbo_static);
    glDeleteBuffers(1, &S.vbo_ring);
    glDeleteVertexArrays(1, &S.vao_static);
    glDeleteVertexArrays(1, &S.vao_ring);
}

static void mix_plan(int n)
{
    uint64_t seed = 0x313;
    int prog = 0;
    for (int i = 0; i < n; i++) {
        mix_op *o = &S.ops[i];
        uint64_t r = rng_next(&seed);
        o->textured = (r % 100) < 82;
        if (((r >> 8) % 100) < 10)
            prog = (int)((r >> 16) % 4);
        o->prog = (uint8_t)prog;
        o->blended = ((r >> 24) % 100) < 74;
        o->tex = o->textured && ((r >> 32) % 100) < 90 ? (uint8_t)((r >> 40) % 64) : 255;
        o->rt = (uint8_t)((i * 6 / n) & 1); /* 6 RT switches per frame */
    }
}

/* GS PACKED quad (4 x 48 bytes) -> 4 GL vertices (x, y, u, v) */
static void gs_to_gl(const uint32_t *g, float *o)
{
    for (int v = 0; v < 4; v++, g += 12, o += 4) {
        float s, t, q;
        memcpy(&s, &g[0], 4);
        memcpy(&t, &g[1], 4);
        memcpy(&q, &g[2], 4);
        float x = (float)(int32_t)(g[8] & 0xFFFF) * (1.0f / 16.0f) - (2048.0f - RT_W / 2);
        float y = (float)(int32_t)(g[9] & 0xFFFF) * (1.0f / 16.0f) - (2048.0f - RT_H / 2);
        o[0] = x * (2.0f / RT_W) - 1.0f;
        o[1] = y * (2.0f / RT_H) - 1.0f;
        o[2] = s / q;
        o[3] = t / q;
    }
}

static GLint ring_push(const float *v)
{
    if (S.ring_off + 64 > RING_BYTES) {
        glBufferData(GL_ARRAY_BUFFER, RING_BYTES, NULL, GL_STREAM_DRAW); /* orphan */
        S.ring_off = 0;
    }
    glBufferSubData(GL_ARRAY_BUFFER, S.ring_off, 64, v);
    GLint first = (GLint)(S.ring_off / 16);
    S.ring_off += 64;
    return first;
}

/* conversion output for the gs variants: [frame parity][quad][16 floats] */
static float conv_buf[2][MAXQ * 16];

typedef struct {
    int path, variant, n, gs, frame;
    const float *src; /* GL vertices for this frame (gs variants: conv_buf) */
} draw_cfg;

static void bind_path(int path)
{
    if (path == P_CLIENT) {
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
    } else if (path == P_STREAM) {
        glBindVertexArray(S.vao_ring);
        glBindBuffer(GL_ARRAY_BUFFER, S.vbo_ring);
    } else {
        glBindVertexArray(S.vao_static);
    }
}

/* Issues one frame's draws; returns CPU seconds spent issuing them. */
static double draw_frame(const draw_cfg *c)
{
    int n = c->n, curprog = -1, curblend = -1, currt = 0, curtex = -1;
    glBindFramebuffer(GL_FRAMEBUFFER, S.fb[0].fb);
    glViewport(0, 0, RT_W, RT_H);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    bind_path(c->path);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(S.prog_tex[0]);
    glBindTexture(GL_TEXTURE_2D, S.pool[0]);
    if (c->variant == V_BLEND)
        glEnable(GL_BLEND);
    double t0 = now_s();
    for (int i = 0; i < n; i++) {
        const float *v = c->src + i * 16;
        if (c->gs == 1)
            gs_to_gl(&S.gsverts[i * 48], conv_buf[0] + i * 16), v = conv_buf[0] + i * 16;
        switch (c->variant) {
        case V_UNIFORM:
            glUniform4f(S.ucol_tex[0], 1.0f, (float)(i & 255) / 255.0f, 1.0f, 1.0f);
            break;
        case V_TEXBIND:
            glBindTexture(GL_TEXTURE_2D, S.pool[i & 63]);
            break;
        case V_BLEND:
            if (i & 1)
                glBlendFunc(GL_ONE, GL_ONE);
            else
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            break;
        case V_PROGRAM:
            glUseProgram(S.prog_tex[i & 7]);
            break;
        case V_FBO:
            if (i && i % 50 == 0) {
                currt ^= 1;
                glBindFramebuffer(GL_FRAMEBUFFER, S.fb[currt].fb);
            }
            break;
        case V_MIX: {
            const mix_op *o = &S.ops[i];
            if (o->rt != currt) {
                currt = o->rt;
                glBindFramebuffer(GL_FRAMEBUFFER, S.fb[currt].fb);
            }
            int prog = o->textured ? o->prog : 8;
            if (prog != curprog) {
                glUseProgram(prog == 8 ? S.prog_col : S.prog_tex[prog]);
                curprog = prog;
            }
            if (o->tex != 255 && o->tex != curtex) {
                glBindTexture(GL_TEXTURE_2D, S.pool[o->tex]);
                curtex = o->tex;
            }
            if (o->blended != curblend) {
                if (o->blended) {
                    glEnable(GL_BLEND);
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                } else {
                    glDisable(GL_BLEND);
                }
                curblend = o->blended;
            }
            glUniform4f(prog == 8 ? S.ucol_col : S.ucol_tex[prog], 1.0f, (float)(i & 255) / 255.0f, 1.0f, 1.0f);
            break;
        }
        default:
            break;
        }
        if (c->path == P_CLIENT) {
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, v);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, v + 2);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        } else if (c->path == P_STREAM) {
            glDrawArrays(GL_TRIANGLE_STRIP, ring_push(v), 4);
        } else {
            glDrawArrays(GL_TRIANGLE_STRIP, i * 4, 4);
        }
    }
    double dt = now_s() - t0;
    glBindVertexArray(0);
    return dt;
}

/* producer thread for mix_gs_t2: converts frame k+1 while frame k draws */
typedef struct {
    pthread_barrier_t bar;
    int n, frames;
} producer_ctx;

static void *producer(void *arg)
{
    producer_ctx *p = arg;
    for (int f = 0; f <= p->frames; f++) {
        float *dst = conv_buf[(f + 1) & 1];
        for (int i = 0; i < p->n; i++)
            gs_to_gl(&S.gsverts[i * 48], dst + i * 16);
        pthread_barrier_wait(&p->bar);
    }
    return NULL;
}

static void measure(int path, int variant, int n, int gs)
{
    samples us = { { 0 }, 0 }, fms = { { 0 }, 0 };
    int frames = g_gl.frames;
    for (int r = 0; r < g_opts.reps; r++) {
        draw_cfg c = { path, variant, n, gs == 1 ? 1 : 0, 0, S.verts };
        producer_ctx pc;
        pthread_t th;
        if (gs == 2) {
            for (int i = 0; i < n; i++)
                gs_to_gl(&S.gsverts[i * 48], conv_buf[0] + i * 16);
            pc.n = n;
            pc.frames = frames;
            pthread_barrier_init(&pc.bar, NULL, 2);
            pthread_create(&th, NULL, producer, &pc);
        }
        double submit = 0, total = 0;
        for (int f = 0; f < frames; f++) {
            if (gs == 2)
                c.src = conv_buf[f & 1];
            double t0 = now_s();
            submit += draw_frame(&c);
            glFinish();
            total += now_s() - t0;
            if (gs == 2)
                pthread_barrier_wait(&pc.bar);
            gl_events();
        }
        if (gs == 2) {
            pthread_barrier_wait(&pc.bar); /* producer's final frame */
            pthread_join(th, NULL);
            pthread_barrier_destroy(&pc.bar);
        }
        samples_add(&us, submit / frames / n * 1e6);
        samples_add(&fms, total / frames * 1e3);
    }
    char var[64], par[200];
    const char *vn = gs == 1 ? "mix_gs_t1" : gs == 2 ? "mix_gs_t2" : vname[variant];
    snprintf(var, sizeof var, "%s_%s_n%d", pname[path], vn, n);
    snprintf(par, sizeof par,
             "\"path\":\"%s\",\"variant\":\"%s\",\"n\":%d,\"frames\":%d,\"threshold_us_per_draw\":11,"
             "\"partb_draws\":877",
             pname[path], vn, n, frames);
    result_add("T1", var, "us_per_draw", "us", &us, par);
    result_add("T1", var, "frame_ms", "ms", &fms, par);
}

void t1_run(void)
{
    printf("T1: draw-call submission rate\n");
    setup();
    static const int ns_full[] = { 100, 250, 500, 877, 1000, 2000, 4000 };
    static const int ns_quick[] = { 100, 877 };
    const int *ns = g_opts.quick ? ns_quick : ns_full;
    int nn = g_opts.quick ? 2 : 7;
    for (int k = 0; k < nn; k++) {
        mix_plan(ns[k]);
        for (int p = 0; p < P_COUNT; p++)
            for (int v = 0; v < V_COUNT; v++)
                measure(p, v, ns[k], 0);
        measure(P_STREAM, V_MIX, ns[k], 1);
        measure(P_STREAM, V_MIX, ns[k], 2);
        temps_note("t1");
    }
    gl_check("t1");
    teardown();
}
