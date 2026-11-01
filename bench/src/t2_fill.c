/* T2: fill and blending. Full-screen quads x 1/2/4/8/16 per frame on the
   default framebuffer (the panel) and on a 640x448 offscreen RT (the
   game's main target), per mode:
     opaque, tex_nearest, tex_bilinear (RGBA8 256^2 at ~1 texel/pixel),
     blend_alpha, blend_add, depth24 / depth16 (back to front: every layer
     shades), depth24_f2b (front to back: early-Z can reject),
     ps2blend_fbfetch ((Cs-Cd)*As*2+Cd through framebuffer fetch, if exposed),
     ps2blend_pingpong (the same through an RT copy before every layer,
       copies rotating through a ring of textures; wall time only),
     partb_mix (gameplay mix: ~75 % textured, ~60 % blended, depth on).
   Plus partb_mix at 8 layers with vsync on, for real frame pacing. */
#define _POSIX_C_SOURCE 200809L
#include "gl_bench.h"

#include <stdlib.h>
#include <string.h>

enum { M_OPAQUE, M_NEAREST, M_BILINEAR, M_ALPHA, M_ADD, M_D24, M_D16, M_D24F2B,
       M_FBF, M_PINGPONG, M_MIX, M_COUNT };
static const char *mname[M_COUNT] = { "opaque", "tex_nearest", "tex_bilinear", "blend_alpha",
                                      "blend_add", "depth24", "depth16", "depth24_f2b",
                                      "ps2blend_fbfetch", "ps2blend_pingpong", "partb_mix" };

static const char *FS_COL = "uniform vec4 u_color;\nout vec4 o;\nvoid main() { o = u_color; }\n";
static const char *FS_TEX =
    "uniform sampler2D u_tex;\nuniform vec4 u_color;\nin vec2 v_uv;\nout vec4 o;\n"
    "void main() { o = texture(u_tex, v_uv) * u_color; }\n";
static const char *FS_FBF_EXT =
    "uniform sampler2D u_tex;\nuniform vec4 u_color;\nin vec2 v_uv;\ninout vec4 o;\n"
    "void main() { vec4 s = texture(u_tex, v_uv) * u_color; vec4 d = o;\n"
    "  o = vec4(clamp((s.rgb - d.rgb) * (s.a * 2.0) + d.rgb, 0.0, 1.0), d.a); }\n";
static const char *FS_FBF_ARM =
    "uniform sampler2D u_tex;\nuniform vec4 u_color;\nin vec2 v_uv;\nout vec4 o;\n"
    "void main() { vec4 s = texture(u_tex, v_uv) * u_color; vec4 d = gl_LastFragColorARM;\n"
    "  o = vec4(clamp((s.rgb - d.rgb) * (s.a * 2.0) + d.rgb, 0.0, 1.0), d.a); }\n";
static const char *FS_PINGPONG =
    "uniform sampler2D u_tex;\nuniform sampler2D u_dst;\nuniform vec4 u_color;\nin vec2 v_uv;\nout vec4 o;\n"
    "void main() { vec4 s = texture(u_tex, v_uv) * u_color;\n"
    "  vec4 d = texelFetch(u_dst, ivec2(gl_FragCoord.xy), 0);\n"
    "  o = vec4(clamp((s.rgb - d.rgb) * (s.a * 2.0) + d.rgb, 0.0, 1.0), d.a); }\n";

/* Copies for the ping-pong blend rotate through a ring, so a copy never
   overwrites a texture an in-flight draw may still sample. */
#define PP_RING 48

static struct {
    GLuint p_col, p_tex, p_fbf, p_pp;
    GLuint t_near, t_lin, t_dst[PP_RING];
    unsigned pp_next;
    gl_quad quad_panel, quad_rt;
    gl_fbo rt24, rt16;
    int dst_w, dst_h;
} S;

static void set_color(GLuint p, float a)
{
    glUniform4f(glGetUniformLocation(p, "u_color"), 0.9f, 0.8f, 0.7f, a);
}

static void setup(void)
{
    S.p_col = gl_program(NULL, VS_BASIC, FS_COL);
    S.p_tex = gl_program(NULL, VS_BASIC, FS_TEX);
    glUniform1i(glGetUniformLocation(S.p_tex, "u_tex"), 0);
    if (g_gl.ext_fbf_ext)
        S.p_fbf = gl_program("#extension GL_EXT_shader_framebuffer_fetch : require", VS_BASIC, FS_FBF_EXT);
    else if (g_gl.ext_fbf_arm)
        S.p_fbf = gl_program("#extension GL_ARM_shader_framebuffer_fetch : require", VS_BASIC, FS_FBF_ARM);
    if (S.p_fbf)
        glUniform1i(glGetUniformLocation(S.p_fbf, "u_tex"), 0);
    S.p_pp = gl_program(NULL, VS_BASIC, FS_PINGPONG);
    glUniform1i(glGetUniformLocation(S.p_pp, "u_tex"), 0);
    glUniform1i(glGetUniformLocation(S.p_pp, "u_dst"), 1);
    S.t_near = gl_tex_rgba8(256, 256, 0x21, 0, 1);
    S.t_lin = gl_tex_rgba8(256, 256, 0x22, 1, 1);
    S.dst_w = g_gl.w > 640 ? g_gl.w : 640;
    S.dst_h = g_gl.h > 448 ? g_gl.h : 448;
    glGenTextures(PP_RING, S.t_dst);
    for (int i = 0; i < PP_RING; i++) {
        glBindTexture(GL_TEXTURE_2D, S.t_dst[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, S.dst_w, S.dst_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    gl_quad_init(&S.quad_panel, (float)g_gl.w / 256.0f, (float)g_gl.h / 256.0f);
    gl_quad_init(&S.quad_rt, 640.0f / 256.0f, 448.0f / 256.0f);
    gl_fbo_init(&S.rt24, 640, 448, 24);
    gl_fbo_init(&S.rt16, 640, 448, 16);
    gl_check("t2 setup");
}

static void teardown(void)
{
    glDeleteProgram(S.p_col);
    glDeleteProgram(S.p_tex);
    if (S.p_fbf)
        glDeleteProgram(S.p_fbf);
    glDeleteProgram(S.p_pp);
    glDeleteTextures(1, &S.t_near);
    glDeleteTextures(1, &S.t_lin);
    glDeleteTextures(PP_RING, S.t_dst);
    gl_quad_free(&S.quad_panel);
    gl_quad_free(&S.quad_rt);
    gl_fbo_free(&S.rt24);
    gl_fbo_free(&S.rt16);
    memset(&S, 0, sizeof S);
}

/* one layer's state for mode m, layer index l of `layers` */
static GLuint layer_state(int m, int l, int layers)
{
    GLuint p = S.p_tex;
    glDisable(GL_BLEND);
    glBindTexture(GL_TEXTURE_2D, S.t_near);
    switch (m) {
    case M_OPAQUE:
        p = S.p_col;
        break;
    case M_BILINEAR:
        glBindTexture(GL_TEXTURE_2D, S.t_lin);
        break;
    case M_ALPHA:
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        break;
    case M_ADD:
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        break;
    case M_FBF:
        p = S.p_fbf;
        break;
    case M_PINGPONG:
        p = S.p_pp;
        break;
    case M_MIX: {
        /* per 10 layers: 3 opaque textured, 1 opaque colour, 5 textured
           alpha-blended, 1 colour additive */
        int k = (l * 10) / (layers > 0 ? layers : 1) % 10;
        if (k == 3)
            p = S.p_col;
        else if (k >= 4 && k <= 8) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        } else if (k == 9) {
            p = S.p_col;
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
        }
        break;
    }
    default:
        break;
    }
    glUseProgram(p);
    set_color(p, 0.5f);
    return p;
}

static int mode_uses_depth(int m)
{
    return m == M_D24 || m == M_D16 || m == M_D24F2B || m == M_MIX;
}

/* Draws one frame; target 0 = panel (default FB), 1 = offscreen RT. */
static void frame(int m, int layers, int target)
{
    int w = target ? 640 : g_gl.w, h = target ? 448 : g_gl.h;
    gl_fbo *rt = m == M_D16 ? &S.rt16 : &S.rt24;
    glBindFramebuffer(GL_FRAMEBUFFER, target ? rt->fb : 0);
    glViewport(0, 0, w, h);
    glClearColor(0.1f, 0.2f, 0.3f, 1.0f);
    glClearDepthf(1.0f);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (mode_uses_depth(m)) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    gl_quad *q = target ? &S.quad_rt : &S.quad_panel;
    glBindVertexArray(q->vao);
    glActiveTexture(GL_TEXTURE0);
    for (int l = 0; l < layers; l++) {
        GLuint p = layer_state(m, l, layers);
        float z = m == M_D24F2B ? -0.9f + 1.8f * (float)l / (float)layers   /* nearest first */
                                : 0.9f - 1.8f * (float)l / (float)layers;   /* farthest first */
        glUniform1f(glGetUniformLocation(p, "u_z"), z);
        if (m == M_PINGPONG) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, S.t_dst[S.pp_next++ % PP_RING]);
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
            glActiveTexture(GL_TEXTURE0);
        }
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    if (target) {
        const GLenum discard[] = { GL_DEPTH_ATTACHMENT };
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, discard);
    }
    glBindVertexArray(0);
}

static void measure(int m, int layers, int target)
{
    int w = target ? 640 : g_gl.w, h = target ? 448 : g_gl.h;
    double px = (double)w * h * layers;
    samples wall = { { 0 }, 0 }, gpu = { { 0 }, 0 };
    char var[64], par[260];
    snprintf(var, sizeof var, "%s_%s_x%d", target ? "rt640x448" : "panel", mname[m], layers);
    gl_clear_errors();
    for (int r = 0; r < g_opts.reps; r++) {
        frame(m, layers, target); /* not timed: first-use costs */
        glFinish();
        double t0 = now_s(), gsum = 0;
        int gn = 0;
        for (int f = 0; f < g_gl.frames; f++) {
            GLuint qq = 0;
            /* No timer queries around glCopyTexSubImage2D: on the Mali-G31
               r20p0 driver the pair raises GL_OUT_OF_MEMORY and the frame's
               GPU work is dropped (measured 2026-11-01). Wall time only. */
            if (m != M_PINGPONG)
                gl_timer_begin(&qq);
            frame(m, layers, target);
            double g = gl_timer_end_ms(qq);
            if (g > 0)
                gsum += g, gn++;
            if (!target)
                gl_swap();
            gl_events();
        }
        glFinish();
        double dt = (now_s() - t0) / g_gl.frames;
        samples_add(&wall, px / dt / 1e6);
        if (gn)
            samples_add(&gpu, px / (gsum / gn / 1e3) / 1e6);
    }
    unsigned err = gl_take_error(var);
    snprintf(par, sizeof par,
             "\"mode\":\"%s\",\"layers\":%d,\"target\":\"%s\",\"w\":%d,\"h\":%d,"
             "\"partb_gameplay_mpix_s\":95,\"partb_max_mpix_s\":155,\"gl_error\":\"0x%04x\"",
             mname[m], layers, target ? "rt" : "panel", w, h, err);
    result_add("T2", var, "mpix_per_s_wall", "Mpix/s", &wall, par);
    if (gpu.n)
        result_add("T2", var, "mpix_per_s_gpu_timer", "Mpix/s", &gpu, par);
    if (m == M_MIX)
        result_add1("T2", var, "gameplay_p95_gpu_fraction", "fraction", 95.0 / samples_median(&wall), par);
}

static void vsync_run(void)
{
    int frames = g_opts.quick ? 10 : 120;
    sdl.GL_SetSwapInterval(1);
    double *iv = malloc(sizeof(double) * (size_t)frames);
    double prev = now_s();
    for (int f = 0; f < frames; f++) {
        frame(M_MIX, 8, 0);
        gl_swap();
        double t = now_s();
        iv[f] = (t - prev) * 1e3;
        prev = t;
        gl_events();
    }
    sdl.GL_SetSwapInterval(0);
    samples s = { { 0 }, 0 };
    double sum = 0, worst = 0;
    for (int f = 1; f < frames; f++) {
        sum += iv[f];
        worst = iv[f] > worst ? iv[f] : worst;
    }
    double mean = sum / (frames - 1);
    samples_add(&s, mean);
    result_add("T2", "panel_partb_mix_x8_vsync", "frame_interval_mean_ms", "ms", &s, "\"vsync\":1");
    result_add1("T2", "panel_partb_mix_x8_vsync", "frame_interval_max_ms", "ms", worst, "\"vsync\":1");
    free(iv);
}

void t2_run(void)
{
    printf("T2: fill and blending\n");
    setup();
    static const int layers_full[] = { 1, 2, 4, 8, 16 };
    static const int layers_quick[] = { 1, 8 };
    const int *ls = g_opts.quick ? layers_quick : layers_full;
    int nl = g_opts.quick ? 2 : 5;
    note_num("t2.fbfetch", S.p_fbf != 0);
    const char *only = getenv("BENCH_T2_ONLY"); /* debug switch: one mode */
    for (int target = 0; target < 2; target++)
        for (int m = 0; m < M_COUNT; m++) {
            if (only && !strstr(mname[m], only))
                continue;
            if (m == M_FBF && !S.p_fbf)
                continue;
            if (m == M_D16 && !target)
                continue; /* the panel's depth format is the context's */
            for (int k = 0; k < nl; k++)
                measure(m, ls[k], target);
        }
    vsync_run();
    gl_check("t2");
    temps_note("t2");
    teardown();
}
