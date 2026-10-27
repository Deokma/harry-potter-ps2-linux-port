/* Runtime loading of SDL2 (the device firmware's own copy). */
#define _POSIX_C_SOURCE 200809L
#include "sdl_min.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

bsdl_api sdl;

int sdl_load(char *err, size_t n)
{
    static const char *names[] = { "libSDL2-2.0.so.0", "libSDL2-2.0.so", "libSDL2.so" };
    void *h = NULL;
    for (size_t i = 0; i < sizeof names / sizeof *names && !h; i++)
        h = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        snprintf(err, n, "dlopen SDL2 failed: %s", dlerror());
        return -1;
    }
#define SYM(field, name)                                                  \
    do {                                                                  \
        void *p = dlsym(h, name);                                         \
        if (!p) {                                                         \
            snprintf(err, n, "SDL2 symbol %s missing", name);             \
            return -1;                                                    \
        }                                                                 \
        memcpy(&sdl.field, &p, sizeof p);                                 \
    } while (0)
    SYM(Init, "SDL_Init");
    SYM(Quit, "SDL_Quit");
    SYM(GetError, "SDL_GetError");
    SYM(GetVersion, "SDL_GetVersion");
    SYM(GetCurrentVideoDriver, "SDL_GetCurrentVideoDriver");
    SYM(GetCurrentAudioDriver, "SDL_GetCurrentAudioDriver");
    SYM(CreateWindow, "SDL_CreateWindow");
    SYM(DestroyWindow, "SDL_DestroyWindow");
    SYM(ShowCursor, "SDL_ShowCursor");
    SYM(GL_SetAttribute, "SDL_GL_SetAttribute");
    SYM(GL_CreateContext, "SDL_GL_CreateContext");
    SYM(GL_DeleteContext, "SDL_GL_DeleteContext");
    SYM(GL_GetProcAddress, "SDL_GL_GetProcAddress");
    SYM(GL_SwapWindow, "SDL_GL_SwapWindow");
    SYM(GL_SetSwapInterval, "SDL_GL_SetSwapInterval");
    SYM(GL_GetDrawableSize, "SDL_GL_GetDrawableSize");
    SYM(PollEvent, "SDL_PollEvent");
    SYM(OpenAudioDevice, "SDL_OpenAudioDevice");
    SYM(PauseAudioDevice, "SDL_PauseAudioDevice");
    SYM(CloseAudioDevice, "SDL_CloseAudioDevice");
#undef SYM
    return 0;
}
