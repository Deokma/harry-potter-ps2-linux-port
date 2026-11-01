#define _POSIX_C_SOURCE 200809L
#include "gl_bench.h"

#include <stdlib.h>
#include <string.h>

gl_env g_gl;

int gl_events(void)
{
    bsdl_event e;
    int quit = 0;
    while (sdl.PollEvent(&e))
        if (e.type == BSDL_QUIT)
            quit = 1;
    return quit;
}

void gl_swap(void)
{
    sdl.GL_SwapWindow(g_gl.win);
}

void gl_check(const char *where)
{
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
        fprintf(stderr, "GL error 0x%04x at %s\n", e, where);
        note_str("gl.error", where);
    }
}

void gl_clear_errors(void)
{
    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; i++)
        ;
}

unsigned gl_take_error(const char *where)
{
    GLenum first = glGetError();
    if (first == GL_NO_ERROR)
        return 0;
    gl_clear_errors();
    fprintf(stderr, "GL error 0x%04x in %s\n", first, where);
    note_str("gl.error", where);
    return first;
}

const char *const VS_BASIC =
    "layout(location = 0) in vec2 a_pos;\n"
    "layout(location = 1) in vec2 a_uv;\n"
    "uniform vec4 u_xform;\n"
    "uniform float u_z;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  v_uv = a_uv;\n"
    "  gl_Position = vec4(a_pos * u_xform.xy + u_xform.zw, u_z, 1.0);\n"
    "}\n";

static GLuint compile(GLenum type, const char *ext, const char *body)
{
    char head[256];
    snprintf(head, sizeof head, "#version 300 es\n%s%sprecision highp float;\nprecision highp int;\n",
             ext ? ext : "", ext ? "\n" : "");
    const char *src[2] = { head, body };
    GLuint s = glCreateShader(type);
    glShaderSource(s, 2, src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "shader compile failed:\n%s%s\n%s\n", head, body, log);
        exit(3);
    }
    return s;
}

GLuint gl_program(const char *ext, const char *vs_body, const char *fs_body)
{
    GLuint vs = compile(GL_VERTEX_SHADER, NULL, vs_body);
    GLuint fs = compile(GL_FRAGMENT_SHADER, ext, fs_body);
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "program link failed:\n%s\n", log);
        exit(3);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    glUseProgram(p);
    glUniform4f(glGetUniformLocation(p, "u_xform"), 1, 1, 0, 0);
    glUniform1f(glGetUniformLocation(p, "u_z"), 0);
    return p;
}

void gl_quad_init(gl_quad *q, float sx, float sy)
{
    const float v[16] = { -1, -1, 0, 0, 1, -1, sx, 0, -1, 1, 0, sy, 1, 1, sx, sy };
    glGenVertexArrays(1, &q->vao);
    glBindVertexArray(q->vao);
    glGenBuffers(1, &q->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, q->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
}

void gl_quad_free(gl_quad *q)
{
    glDeleteBuffers(1, &q->vbo);
    glDeleteVertexArrays(1, &q->vao);
}

static GLuint tex_make(int w, int h, GLenum ifmt, GLenum fmt, size_t bpp, uint64_t seed, int linear, int repeat)
{
    uint8_t *data = malloc((size_t)w * h * bpp);
    for (size_t i = 0; i < (size_t)w * h * bpp; i++)
        data[i] = (uint8_t)(rng_next(&seed) >> 32);
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)ifmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    free(data);
    return t;
}

GLuint gl_tex_rgba8(int w, int h, uint64_t seed, int linear, int repeat)
{
    return tex_make(w, h, GL_RGBA8, GL_RGBA, 4, seed, linear, repeat);
}

GLuint gl_tex_r8(int w, int h, uint64_t seed)
{
    return tex_make(w, h, GL_R8, GL_RED, 1, seed, 0, 1);
}

void gl_fbo_init(gl_fbo *f, int w, int h, int depth)
{
    memset(f, 0, sizeof *f);
    f->w = w;
    f->h = h;
    glGenTextures(1, &f->color);
    glBindTexture(GL_TEXTURE_2D, f->color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &f->fb);
    glBindFramebuffer(GL_FRAMEBUFFER, f->fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, f->color, 0);
    if (depth) {
        glGenRenderbuffers(1, &f->depth);
        glBindRenderbuffer(GL_RENDERBUFFER, f->depth);
        glRenderbufferStorage(GL_RENDERBUFFER, depth == 16 ? GL_DEPTH_COMPONENT16 : GL_DEPTH24_STENCIL8, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, depth == 16 ? GL_DEPTH_ATTACHMENT : GL_DEPTH_STENCIL_ATTACHMENT,
                                  GL_RENDERBUFFER, f->depth);
    }
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE)
        fprintf(stderr, "FBO %dx%d depth %d incomplete: 0x%04x\n", w, h, depth, st);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void gl_fbo_free(gl_fbo *f)
{
    glDeleteFramebuffers(1, &f->fb);
    glDeleteTextures(1, &f->color);
    if (f->depth)
        glDeleteRenderbuffers(1, &f->depth);
    memset(f, 0, sizeof *f);
}

void gl_timer_begin(GLuint *q)
{
    static int off = -1; /* debug switch: BENCH_NO_TIMER=1 */
    if (off < 0)
        off = getenv("BENCH_NO_TIMER") != NULL;
    *q = 0;
    if (off || !g_gl.ext_timer || !glGetQueryObjectui64vEXT)
        return;
    GLint disjoint;
    glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint); /* clears the flag */
    glGenQueries(1, q);
    glBeginQuery(GL_TIME_ELAPSED_EXT, *q);
}

double gl_timer_end_ms(GLuint q)
{
    if (!q)
        return -1;
    glEndQuery(GL_TIME_ELAPSED_EXT);
    GLuint avail = 0;
    double t0 = now_s();
    while (!avail && now_s() - t0 < 2.0)
        glGetQueryObjectuiv(q, GL_QUERY_RESULT_AVAILABLE, &avail);
    GLuint64 ns = 0;
    GLint disjoint = 0;
    if (avail)
        glGetQueryObjectui64vEXT(q, GL_QUERY_RESULT, &ns);
    glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
    glDeleteQueries(1, &q);
    return avail && !disjoint ? (double)ns / 1e6 : -1;
}
