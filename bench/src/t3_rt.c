/* T3: render targets and readback on a tiler.
   - FBO switch with a small draw on each side: keep (no clear), clear
     after bind, invalidate depth before leaving;
   - RT -> texture copies (glCopyTexSubImage2D, glBlitFramebuffer),
     64^2 .. 640x448;
   - glReadPixels of 4 KB and of the whole 640x448 RT, right after a draw
     (includes the pipeline drain). */
#define _POSIX_C_SOURCE 200809L
#include "gl_bench.h"

#include <stdlib.h>
#include <string.h>

static const char *FS_COL = "uniform vec4 u_color;\nout vec4 o;\nvoid main() { o = u_color; }\n";

static struct {
    GLuint prog;
    gl_quad quad;
    gl_fbo a, b, dst;
    GLuint tex;
} S;

static void small_draw(float x)
{
    glUniform4f(glGetUniformLocation(S.prog, "u_xform"), 0.05f, 0.05f, x, 0.0f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void full_draw(void)
{
    glUniform4f(glGetUniformLocation(S.prog, "u_xform"), 1, 1, 0, 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void switch_test(int mode)
{
    static const char *names[] = { "keep", "clear", "invalidate_depth" };
    const int m = g_opts.quick ? 20 : 200;
    samples us = { { 0 }, 0 };
    const GLenum depth[] = { GL_DEPTH_STENCIL_ATTACHMENT };
    for (int r = 0; r < g_opts.reps; r++) {
        glFinish();
        double t0 = now_s();
        for (int i = 0; i < m; i++) {
            gl_fbo *f = i & 1 ? &S.b : &S.a;
            glBindFramebuffer(GL_FRAMEBUFFER, f->fb);
            if (mode == 1)
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            small_draw((float)(i % 16) / 16.0f - 0.5f);
            if (mode == 2)
                glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, depth);
        }
        glFinish();
        samples_add(&us, (now_s() - t0) / m * 1e6);
        gl_events();
    }
    char var[48];
    snprintf(var, sizeof var, "fbo_switch_%s", names[mode]);
    result_add("T3", var, "us_per_switch", "us", &us,
               "\"rt\":\"640x448 RGBA8 + D24S8\",\"threshold_us\":500,\"partb_switches\":6");
}

static void copy_test(int blit, int w, int h)
{
    const int m = g_opts.quick ? 5 : 50;
    samples us = { { 0 }, 0 }, mbs = { { 0 }, 0 };
    for (int r = 0; r < g_opts.reps; r++) {
        glFinish();
        double t0 = now_s();
        for (int i = 0; i < m; i++) {
            glBindFramebuffer(GL_FRAMEBUFFER, S.a.fb);
            small_draw((float)(i % 16) / 16.0f - 0.5f);
            if (blit) {
                glBindFramebuffer(GL_READ_FRAMEBUFFER, S.a.fb);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, S.dst.fb);
                glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            } else {
                glBindTexture(GL_TEXTURE_2D, S.tex);
                glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
            }
        }
        glFinish();
        double dt = (now_s() - t0) / m;
        samples_add(&us, dt * 1e6);
        samples_add(&mbs, (double)w * h * 4 / dt / 1e6);
        gl_events();
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    char var[48], par[64];
    snprintf(var, sizeof var, "%s_%dx%d", blit ? "blit" : "copytexsubimage", w, h);
    snprintf(par, sizeof par, "\"w\":%d,\"h\":%d", w, h);
    result_add("T3", var, "us_per_copy", "us", &us, par);
    result_add("T3", var, "MBps", "MB/s", &mbs, par);
}

static void readback_test(int w, int h)
{
    const int m = g_opts.quick ? 3 : 20;
    uint8_t *buf = malloc((size_t)w * h * 4);
    samples ms = { { 0 }, 0 }, mbs = { { 0 }, 0 };
    glBindFramebuffer(GL_FRAMEBUFFER, S.a.fb);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    for (int r = 0; r < g_opts.reps; r++) {
        double sum = 0;
        for (int i = 0; i < m; i++) {
            full_draw();
            double t0 = now_s();
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
            sum += now_s() - t0;
        }
        samples_add(&ms, sum / m * 1e3);
        samples_add(&mbs, (double)w * h * 4 / (sum / m) / 1e6);
        gl_events();
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    free(buf);
    char var[48], par[64];
    snprintf(var, sizeof var, "readpixels_%dx%d", w, h);
    snprintf(par, sizeof par, "\"w\":%d,\"h\":%d,\"after_full_draw\":true", w, h);
    result_add("T3", var, "ms_per_read", "ms", &ms, par);
    result_add("T3", var, "MBps", "MB/s", &mbs, par);
}

void t3_run(void)
{
    printf("T3: render targets and readback\n");
    S.prog = gl_program(NULL, VS_BASIC, FS_COL);
    glUniform4f(glGetUniformLocation(S.prog, "u_color"), 0.3f, 0.6f, 0.9f, 1.0f);
    gl_quad_init(&S.quad, 1, 1);
    gl_fbo_init(&S.a, 640, 448, 24);
    gl_fbo_init(&S.b, 640, 448, 24);
    gl_fbo_init(&S.dst, 640, 448, 0);
    glGenTextures(1, &S.tex);
    glBindTexture(GL_TEXTURE_2D, S.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 640, 448, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glViewport(0, 0, 640, 448);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glBindVertexArray(S.quad.vao);
    for (int mode = 0; mode < 3; mode++)
        switch_test(mode);
    static const int sz[][2] = { { 64, 64 }, { 128, 128 }, { 256, 256 }, { 640, 448 } };
    for (int blit = 0; blit < 2; blit++)
        for (int i = 0; i < 4; i++)
            copy_test(blit, sz[i][0], sz[i][1]);
    readback_test(32, 32);
    readback_test(640, 448);
    glBindVertexArray(0);
    gl_check("t3");
    temps_note("t3");
    glDeleteProgram(S.prog);
    gl_quad_free(&S.quad);
    gl_fbo_free(&S.a);
    gl_fbo_free(&S.b);
    gl_fbo_free(&S.dst);
    glDeleteTextures(1, &S.tex);
    memset(&S, 0, sizeof S);
}
