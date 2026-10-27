/* Shared state and helpers for the GLES tests in bench_gl. */
#ifndef GL_BENCH_H
#define GL_BENCH_H

#include "bench.h"
#include "gl_dyn.h"
#include "sdl_min.h"

typedef struct {
    bsdl_window *win;
    int w, h;            /* drawable size of the default framebuffer */
    int ext_timer;       /* EXT_disjoint_timer_query */
    int ext_fbf_ext;     /* EXT_shader_framebuffer_fetch */
    int ext_fbf_arm;     /* ARM_shader_framebuffer_fetch */
    int frames;          /* frames per measurement */
} gl_env;

extern gl_env g_gl;

int gl_events(void);           /* pump SDL events; 1 if the user quit */
void gl_swap(void);
void gl_check(const char *where);

/* Program from GLSL ES 3.00 bodies; the "#version 300 es" header and
   precision are added. `ext` is an optional #extension line. Attribute 0
   is a_pos (vec2), 1 is a_uv (vec2). Aborts on compile/link errors. */
GLuint gl_program(const char *ext, const char *vs_body, const char *fs_body);
extern const char *const VS_BASIC;   /* u_xform (scale xy, offset zw), u_z; v_uv */

/* full-screen quad (triangle strip, 4 vertices, pos + uv) in a VBO + VAO */
typedef struct {
    GLuint vbo, vao;
} gl_quad;
void gl_quad_init(gl_quad *q, float uv_scale_x, float uv_scale_y);
void gl_quad_free(gl_quad *q);

GLuint gl_tex_rgba8(int w, int h, uint64_t seed, int linear, int repeat);
GLuint gl_tex_r8(int w, int h, uint64_t seed);

typedef struct {
    GLuint fb, color, depth;  /* color is a texture, depth a renderbuffer */
    int w, h;
} gl_fbo;
/* depth: 0 none, 16 = D16, 24 = D24S8 */
void gl_fbo_init(gl_fbo *f, int w, int h, int depth);
void gl_fbo_free(gl_fbo *f);

/* GPU time of the work between begin and end, in ms; < 0 if the timer
   query is unavailable or the result was disjoint. */
void gl_timer_begin(GLuint *q);
double gl_timer_end_ms(GLuint q);

/* tests */
void t1_run(void);
void t2_run(void);
void t3_run(void);
void t4_run(void);
void t5_gpu_run(void);
void t8_audio_run(double seconds);

#endif
