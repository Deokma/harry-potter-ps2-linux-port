/* bench_gl: the GLES tests (T1 draw rate, T2 fill, T3 render targets,
   T4 upload, T5 GPU palette path) and T8 audio. SDL2 is dlopened from the
   firmware; GL entry points come from SDL_GL_GetProcAddress. */
#define _POSIX_C_SOURCE 200809L
#include "gl_bench.h"

#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    puts("usage: bench_gl [--quick] [--reps N] [--warmup S] [--out DIR] [--tests T1,T2,T4,T5,T3,T8]\n"
         "                [--windowed] [--audio-s S]\n"
         "  --quick     few frames, 1 rep, no warm-up (desktop smoke run; numbers mean nothing)\n"
         "  --windowed  640x480 window instead of fullscreen\n"
         "Results: <out>/<date>-<firmware>-gl.json and .csv (default out: results)");
}

static int has_test(const char *list, const char *id)
{
    size_t n = strlen(id);
    for (const char *p = list; (p = strstr(p, id)); p += n)
        if ((p == list || p[-1] == ',') && (p[n] == 0 || p[n] == ','))
            return 1;
    return 0;
}

static void gpu_warmup(double seconds)
{
    if (seconds <= 0)
        return;
    printf("warm-up: %.0f s of full-screen fill\n", seconds);
    GLuint p = gl_program(NULL, VS_BASIC, "uniform vec4 u_color;\nout vec4 o;\nvoid main() { o = u_color; }\n");
    glUniform4f(glGetUniformLocation(p, "u_color"), 0.5f, 0.5f, 0.5f, 1.0f);
    gl_quad q;
    gl_quad_init(&q, 1, 1);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, g_gl.w, g_gl.h);
    double t0 = now_s();
    while (now_s() - t0 < seconds) {
        for (int l = 0; l < 8; l++)
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        gl_swap();
        gl_events();
    }
    glFinish();
    glBindVertexArray(0);
    gl_quad_free(&q);
    glDeleteProgram(p);
}

static void note_gl_strings(void)
{
    note_str("gl.vendor", (const char *)glGetString(GL_VENDOR));
    note_str("gl.renderer", (const char *)glGetString(GL_RENDERER));
    note_str("gl.version", (const char *)glGetString(GL_VERSION));
    note_str("gl.glsl_version", (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));
    GLint n = 0, v = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    size_t cap = 16384, len = 0;
    char *all = malloc(cap);
    all[0] = 0;
    for (GLint i = 0; i < n; i++) {
        const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
        size_t el = e ? strlen(e) : 0;
        if (len + el + 2 >= cap)
            all = realloc(all, cap *= 2);
        if (e) {
            memcpy(all + len, e, el);
            len += el;
            all[len++] = ' ';
            all[len] = 0;
        }
    }
    note_str("gl.extensions", all);
    free(all);
    note_num("gl.num_extensions", n);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &v);
    note_num("gl.max_texture_size", v);
    glGetIntegerv(GL_DEPTH_BITS, &v);
    note_num("gl.default_fb_depth_bits", v);
}

int main(int argc, char **argv)
{
    const char *tests = "T1,T2,T4,T5,T3,T8";
    int windowed = 0;
    double audio_s = 60;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--quick"))
            g_opts.quick = 1;
        else if (!strcmp(a, "--reps") && v)
            g_opts.reps = atoi(argv[++i]);
        else if (!strcmp(a, "--warmup") && v)
            g_opts.warmup_s = strtod(argv[++i], NULL);
        else if (!strcmp(a, "--out") && v)
            g_opts.out_dir = argv[++i];
        else if (!strcmp(a, "--tests") && v)
            tests = argv[++i];
        else if (!strcmp(a, "--windowed"))
            windowed = 1;
        else if (!strcmp(a, "--audio-s") && v)
            audio_s = strtod(argv[++i], NULL);
        else {
            usage();
            return a[0] == '-' && a[1] == 'h' ? 0 : 2;
        }
    }
    if (g_opts.quick) {
        g_opts.reps = 1;
        g_opts.warmup_s = 0;
        audio_s = 3;
    }
    if (g_opts.reps < 1 || g_opts.reps > BENCH_MAX_SAMPLES)
        g_opts.reps = 3;
    g_gl.frames = g_opts.quick ? 2 : 20;
    setvbuf(stdout, NULL, _IOLBF, 0);

    char err[256];
    if (sdl_load(err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    if (sdl.Init(BSDL_INIT_VIDEO | BSDL_INIT_AUDIO | BSDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL_Init: %s\n", sdl.GetError());
        return 1;
    }
    sdl.GL_SetAttribute(BSDL_GL_CONTEXT_PROFILE_MASK, BSDL_GL_CONTEXT_PROFILE_ES);
    sdl.GL_SetAttribute(BSDL_GL_CONTEXT_MAJOR_VERSION, 3);
    sdl.GL_SetAttribute(BSDL_GL_CONTEXT_MINOR_VERSION, 0);
    sdl.GL_SetAttribute(BSDL_GL_RED_SIZE, 8);
    sdl.GL_SetAttribute(BSDL_GL_GREEN_SIZE, 8);
    sdl.GL_SetAttribute(BSDL_GL_BLUE_SIZE, 8);
    sdl.GL_SetAttribute(BSDL_GL_DEPTH_SIZE, 24);
    sdl.GL_SetAttribute(BSDL_GL_STENCIL_SIZE, 8);
    sdl.GL_SetAttribute(BSDL_GL_DOUBLEBUFFER, 1);
    uint32_t flags = BSDL_WINDOW_OPENGL | (windowed ? 0 : BSDL_WINDOW_FULLSCREEN_DESKTOP);
    g_gl.win = sdl.CreateWindow("ps2bench", (int)BSDL_WINDOWPOS_UNDEFINED, (int)BSDL_WINDOWPOS_UNDEFINED, 640,
                                480, flags);
    if (!g_gl.win) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", sdl.GetError());
        return 1;
    }
    bsdl_glcontext ctx = sdl.GL_CreateContext(g_gl.win);
    if (!ctx) {
        fprintf(stderr, "SDL_GL_CreateContext (GLES 3.0): %s\n", sdl.GetError());
        return 1;
    }
    if (gl_load(sdl.GL_GetProcAddress, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    sdl.ShowCursor(0);
    sdl.GL_SetSwapInterval(0);
    sdl.GL_GetDrawableSize(g_gl.win, &g_gl.w, &g_gl.h);
    g_gl.ext_timer = gl_has_ext("GL_EXT_disjoint_timer_query") && glGetQueryObjectui64vEXT;
    g_gl.ext_fbf_ext = gl_has_ext("GL_EXT_shader_framebuffer_fetch");
    g_gl.ext_fbf_arm = gl_has_ext("GL_ARM_shader_framebuffer_fetch");

    sysinfo_collect();
    bsdl_version ver;
    sdl.GetVersion(&ver);
    char b[64];
    snprintf(b, sizeof b, "%u.%u.%u", ver.major, ver.minor, ver.patch);
    note_str("binary", "bench_gl");
    note_str("tests", tests);
    note_str("sdl.version", b);
    note_str("sdl.video_driver", sdl.GetCurrentVideoDriver() ? sdl.GetCurrentVideoDriver() : "none");
    note_num("gl.drawable_w", g_gl.w);
    note_num("gl.drawable_h", g_gl.h);
    note_num("gl.ext_timer_query", g_gl.ext_timer);
    note_num("gl.ext_fbfetch_ext", g_gl.ext_fbf_ext);
    note_num("gl.ext_fbfetch_arm", g_gl.ext_fbf_arm);
    note_gl_strings();
    printf("GL: %s | %s | %s | %dx%d\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER),
           glGetString(GL_VERSION), g_gl.w, g_gl.h);

    gpu_warmup(g_opts.warmup_s);
    if (has_test(tests, "T1"))
        t1_run();
    if (has_test(tests, "T2"))
        t2_run();
    if (has_test(tests, "T4"))
        t4_run();
    if (has_test(tests, "T5"))
        t5_gpu_run();
    if (has_test(tests, "T3"))
        t3_run();
    if (has_test(tests, "T8"))
        t8_audio_run(audio_s);
    temps_note("end");
    int rc = results_write("gl") ? 1 : 0;
    sdl.GL_DeleteContext(ctx);
    sdl.DestroyWindow(g_gl.win);
    sdl.Quit();
    return rc;
}
