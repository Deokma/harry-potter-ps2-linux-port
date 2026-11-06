#include "gsplay_gl.h"

extern "C"
{
#include "gl_dyn.h"
#include "sdl_min.h"
}

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace
{
    bsdl_window *g_window = nullptr;
    int g_width = 0;
    int g_height = 0;
}

int gsplayScreenWidth() { return g_width; }
int gsplayScreenHeight() { return g_height; }

bool gsplayGlInit()
{
    char err[256] = {};
    if (sdl_load(err, sizeof(err)))
    {
        std::fprintf(stderr, "gsplay: SDL2 not loaded: %s\n", err);
        return false;
    }
    if (sdl.Init(BSDL_INIT_VIDEO | BSDL_INIT_EVENTS))
    {
        std::fprintf(stderr, "SDL_Init: %s\n", sdl.GetError());
        return false;
    }
    sdl.GL_SetAttribute(BSDL_GL_CONTEXT_PROFILE_MASK, BSDL_GL_CONTEXT_PROFILE_ES);
    sdl.GL_SetAttribute(BSDL_GL_CONTEXT_MAJOR_VERSION, 3);
    sdl.GL_SetAttribute(BSDL_GL_CONTEXT_MINOR_VERSION, 0);
    sdl.GL_SetAttribute(BSDL_GL_RED_SIZE, 8);
    sdl.GL_SetAttribute(BSDL_GL_GREEN_SIZE, 8);
    sdl.GL_SetAttribute(BSDL_GL_BLUE_SIZE, 8);
    sdl.GL_SetAttribute(BSDL_GL_DOUBLEBUFFER, 1);
    const bool windowed = std::getenv("GSPLAY_WINDOWED") != nullptr;
    const uint32_t flags = BSDL_WINDOW_OPENGL | (windowed ? 0u : BSDL_WINDOW_FULLSCREEN_DESKTOP);
    g_window = sdl.CreateWindow("gsplay", (int)BSDL_WINDOWPOS_UNDEFINED, (int)BSDL_WINDOWPOS_UNDEFINED, 640, 480, flags);
    if (!g_window)
    {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", sdl.GetError());
        return false;
    }
    bsdl_glcontext ctx = sdl.GL_CreateContext(g_window);
    if (!ctx)
    {
        std::fprintf(stderr, "SDL_GL_CreateContext (GLES 3.0): %s\n", sdl.GetError());
        return false;
    }
    if (gl_load(sdl.GL_GetProcAddress, err, sizeof(err)))
    {
        std::fprintf(stderr, "GL load: %s\n", err);
        return false;
    }
    sdl.ShowCursor(0);
    sdl.GL_SetSwapInterval(0);
    sdl.GL_GetDrawableSize(g_window, &g_width, &g_height);
    std::printf("gl: %s, %s; SDL video %s; %dx%d; fetch ARM %d EXT %d\n", glGetString(GL_RENDERER), glGetString(GL_VERSION),
                sdl.GetCurrentVideoDriver() ? sdl.GetCurrentVideoDriver() : "?", g_width, g_height,
                gl_has_ext("GL_ARM_shader_framebuffer_fetch"), gl_has_ext("GL_EXT_shader_framebuffer_fetch"));
    return true;
}

namespace
{
    double g_finishMs = 0, g_swapMs = 0;
    double glNowMs()
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
}

void gsplayGlTimes(double &finishMs, double &swapMs)
{
    finishMs = g_finishMs;
    swapMs = g_swapMs;
    g_finishMs = g_swapMs = 0;
}

void gsplayGlSwap()
{
    // With GSPLAY_FINISH the GPU's work counts in the frame's time.
    static const bool finish = std::getenv("GSPLAY_FINISH") != nullptr;
    const double t0 = glNowMs();
    if (finish)
        glFinish();
    const double t1 = glNowMs();
    // With GSPLAY_NOSWAP there is no flip to wait for, so glFinish measures only the GPU's work: on this
    // fbdev driver a swap paces everything to the display whatever the swap interval says.
    static const bool noSwap = std::getenv("GSPLAY_NOSWAP") != nullptr;
    if (g_window && !noSwap)
        sdl.GL_SwapWindow(g_window);
    g_finishMs += t1 - t0;
    g_swapMs += glNowMs() - t1;
}
