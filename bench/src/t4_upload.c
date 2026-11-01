/* T4: texture upload. glTexSubImage2D of RGBA8 and R8 at 32^2 .. 512^2,
   straight from client memory or through a mapped pixel-unpack buffer.
   Every upload is followed by a tiny draw sampling it, so drivers can't
   defer the copy past the measurement. */
#define _POSIX_C_SOURCE 200809L
#include "gl_bench.h"

#include <stdlib.h>
#include <string.h>

#define POOL 8

static const char *FS_TEX =
    "uniform sampler2D u_tex;\nin vec2 v_uv;\nout vec4 o;\nvoid main() { o = texture(u_tex, v_uv); }\n";

static void upload_test(int size, int r8, int pbo)
{
    const size_t bpp = r8 ? 1 : 4, bytes = (size_t)size * size * bpp;
    /* ~4 MB per measured frame, at least 4 uploads */
    int m = (int)((4u << 20) / bytes);
    m = m < 4 ? 4 : m > 512 ? 512 : m;
    if (g_opts.quick)
        m = m > 16 ? 16 : m;
    GLuint tex[POOL], buf = 0;
    glGenTextures(POOL, tex);
    for (int i = 0; i < POOL; i++) {
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, r8 ? GL_R8 : GL_RGBA8, size, size, 0, r8 ? GL_RED : GL_RGBA,
                     GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    uint8_t *src = malloc(bytes);
    uint64_t seed = 0x44 + (uint64_t)size;
    for (size_t i = 0; i < bytes; i++)
        src[i] = (uint8_t)(rng_next(&seed) >> 40);
    if (pbo) {
        glGenBuffers(1, &buf);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buf);
        glBufferData(GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr)bytes, NULL, GL_STREAM_DRAW);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    char var[48], par[220];
    snprintf(var, sizeof var, "%s_%d_%s", r8 ? "r8" : "rgba8", size, pbo ? "pbo" : "direct");
    unsigned err = gl_take_error(var); /* allocation of the pool / buffer */
    samples us = { { 0 }, 0 }, mbs = { { 0 }, 0 };
    for (int r = 0; r < g_opts.reps; r++) {
        glFinish();
        double t0 = now_s();
        for (int i = 0; i < m; i++) {
            src[(size_t)i % bytes] ^= 1; /* new contents every time */
            glBindTexture(GL_TEXTURE_2D, tex[i % POOL]);
            if (pbo) {
                glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buf);
                void *p = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, (GLsizeiptr)bytes,
                                           GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
                if (p)
                    memcpy(p, src, bytes);
                glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size, size, r8 ? GL_RED : GL_RGBA, GL_UNSIGNED_BYTE,
                                (void *)0);
                glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size, size, r8 ? GL_RED : GL_RGBA, GL_UNSIGNED_BYTE,
                                src);
            }
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        glFinish();
        double dt = now_s() - t0;
        samples_add(&us, dt / m * 1e6);
        samples_add(&mbs, (double)bytes * m / dt / 1e6);
        gl_events();
    }
    unsigned err2 = gl_take_error(var); /* the uploads themselves */
    snprintf(par, sizeof par,
             "\"format\":\"%s\",\"size\":%d,\"path\":\"%s\",\"uploads\":%d,"
             "\"partb_new_rgba8_bytes\":1.6e6,\"threshold_ms\":2,\"gl_error\":\"0x%04x\"",
             r8 ? "r8" : "rgba8", size, pbo ? "pbo" : "direct", m, err ? err : err2);
    result_add("T4", var, "us_per_upload", "us", &us, par);
    result_add("T4", var, "MBps", "MB/s", &mbs, par);
    if (!r8)
        result_add1("T4", var, "ms_per_1.6MB_rgba8", "ms", 1.6 / samples_median(&mbs) * 1e3, par);
    free(src);
    if (buf)
        glDeleteBuffers(1, &buf);
    glDeleteTextures(POOL, tex);
}

void t4_run(void)
{
    printf("T4: texture upload\n");
    GLuint prog = gl_program(NULL, VS_BASIC, FS_TEX);
    glUniform1i(glGetUniformLocation(prog, "u_tex"), 0);
    /* the sampling draw: a 1/32-screen quad in a 640x448 RT */
    glUniform4f(glGetUniformLocation(prog, "u_xform"), 0.03f, 0.03f, 0.0f, 0.0f);
    gl_quad q;
    gl_quad_init(&q, 1, 1);
    gl_fbo rt;
    gl_fbo_init(&rt, 640, 448, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, rt.fb);
    glViewport(0, 0, 640, 448);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(q.vao);
    gl_check("t4 setup");
    static const int sizes[] = { 32, 64, 128, 256, 512 };
    for (int r8 = 0; r8 < 2; r8++)
        for (int s = 0; s < 5; s++)
            for (int pbo = 0; pbo < 2; pbo++)
                upload_test(sizes[s], r8, pbo);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl_check("t4");
    temps_note("t4");
    gl_fbo_free(&rt);
    gl_quad_free(&q);
    glDeleteProgram(prog);
}
