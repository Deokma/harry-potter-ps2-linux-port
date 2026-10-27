/* T5, GPU path: palettized textures kept as R8 indices plus a 256x1 RGBA8
   palette, looked up in the fragment shader; fill rate compared with plain
   RGBA8 texturing on the same layers. The CPU path is in t5_cpu.c. */
#define _POSIX_C_SOURCE 200809L
#include "gl_bench.h"

#include <string.h>

static const char *FS_RGBA =
    "uniform sampler2D u_tex;\nin vec2 v_uv;\nout vec4 o;\nvoid main() { o = texture(u_tex, v_uv); }\n";
static const char *FS_PAL =
    "uniform sampler2D u_idx;\nuniform sampler2D u_pal;\nin vec2 v_uv;\nout vec4 o;\n"
    "void main() { int i = int(texture(u_idx, v_uv).r * 255.0 + 0.5);\n"
    "  o = texelFetch(u_pal, ivec2(i, 0), 0); }\n";

static double fill_rate(GLuint prog, int layers, const gl_quad *q, int blend)
{
    glUseProgram(prog);
    glBindVertexArray(q->vao);
    if (blend) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }
    for (int l = 0; l < layers; l++)
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();
    double t0 = now_s();
    for (int f = 0; f < g_gl.frames; f++) {
        glClear(GL_COLOR_BUFFER_BIT);
        for (int l = 0; l < layers; l++)
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        gl_events();
    }
    glFinish();
    double dt = (now_s() - t0) / g_gl.frames;
    return 640.0 * 448.0 * layers / dt / 1e6;
}

void t5_gpu_run(void)
{
    printf("T5 GPU: palette lookup in the fragment shader\n");
    GLuint p_rgba = gl_program(NULL, VS_BASIC, FS_RGBA);
    glUniform1i(glGetUniformLocation(p_rgba, "u_tex"), 0);
    GLuint p_pal = gl_program(NULL, VS_BASIC, FS_PAL);
    glUniform1i(glGetUniformLocation(p_pal, "u_idx"), 0);
    glUniform1i(glGetUniformLocation(p_pal, "u_pal"), 1);
    GLuint t_rgba = gl_tex_rgba8(256, 256, 0x51, 0, 1);
    GLuint t_idx = gl_tex_r8(256, 256, 0x52);
    GLuint t_pal = gl_tex_rgba8(256, 1, 0x53, 0, 0);
    gl_quad q;
    gl_quad_init(&q, 640.0f / 256.0f, 448.0f / 256.0f);
    gl_fbo rt;
    gl_fbo_init(&rt, 640, 448, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, rt.fb);
    glViewport(0, 0, 640, 448);
    glDisable(GL_DEPTH_TEST);
    const int layers = 8;
    for (int blend = 0; blend < 2; blend++) {
        samples rgba = { { 0 }, 0 }, pal = { { 0 }, 0 }, ratio = { { 0 }, 0 };
        for (int r = 0; r < g_opts.reps; r++) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, t_rgba);
            double a = fill_rate(p_rgba, layers, &q, blend);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, t_pal);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, t_idx);
            double b = fill_rate(p_pal, layers, &q, blend);
            samples_add(&rgba, a);
            samples_add(&pal, b);
            samples_add(&ratio, b / a);
        }
        const char *par = blend ? "\"layers\":8,\"blend\":\"alpha\"" : "\"layers\":8,\"blend\":\"none\"";
        result_add("T5", blend ? "gpu_rgba8_blend" : "gpu_rgba8", "mpix_per_s", "Mpix/s", &rgba, par);
        result_add("T5", blend ? "gpu_palette_blend" : "gpu_palette", "mpix_per_s", "Mpix/s", &pal, par);
        result_add("T5", blend ? "gpu_palette_blend" : "gpu_palette", "fill_ratio_vs_rgba8", "ratio", &ratio, par);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindVertexArray(0);
    gl_check("t5 gpu");
    temps_note("t5");
    gl_fbo_free(&rt);
    gl_quad_free(&q);
    GLuint t[3] = { t_rgba, t_idx, t_pal };
    glDeleteTextures(3, t);
    glDeleteProgram(p_rgba);
    glDeleteProgram(p_pal);
}
