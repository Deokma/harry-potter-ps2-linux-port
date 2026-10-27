/* The slice of the SDL2 ABI bench_gl uses, declared by hand. SDL2 is
   dlopened on the device, so the cross build needs no SDL headers or
   libraries. sdl_check.c compares every value here with the real SDL2
   headers in native builds. */
#ifndef SDL_MIN_H
#define SDL_MIN_H

#include <stddef.h>
#include <stdint.h>

#define BSDL_INIT_AUDIO 0x00000010u
#define BSDL_INIT_VIDEO 0x00000020u
#define BSDL_INIT_EVENTS 0x00004000u

#define BSDL_WINDOW_FULLSCREEN 0x00000001u
#define BSDL_WINDOW_OPENGL 0x00000002u
#define BSDL_WINDOW_FULLSCREEN_DESKTOP (BSDL_WINDOW_FULLSCREEN | 0x00001000u)
#define BSDL_WINDOWPOS_UNDEFINED 0x1FFF0000u

/* SDL_GLattr */
#define BSDL_GL_RED_SIZE 0
#define BSDL_GL_GREEN_SIZE 1
#define BSDL_GL_BLUE_SIZE 2
#define BSDL_GL_ALPHA_SIZE 3
#define BSDL_GL_DOUBLEBUFFER 5
#define BSDL_GL_DEPTH_SIZE 6
#define BSDL_GL_STENCIL_SIZE 7
#define BSDL_GL_CONTEXT_MAJOR_VERSION 17
#define BSDL_GL_CONTEXT_MINOR_VERSION 18
#define BSDL_GL_CONTEXT_PROFILE_MASK 21
#define BSDL_GL_CONTEXT_PROFILE_ES 0x0004

#define BSDL_QUIT 0x100u
#define BSDL_KEYDOWN 0x300u

#define BSDL_AUDIO_S16LSB 0x8010u

typedef struct bsdl_window bsdl_window;
typedef void *bsdl_glcontext;
typedef uint32_t bsdl_audio_device;

typedef struct {
    uint8_t major, minor, patch;
} bsdl_version;

typedef void (*bsdl_audio_cb)(void *userdata, uint8_t *stream, int len);

typedef struct {
    int freq;
    uint16_t format;
    uint8_t channels;
    uint8_t silence;
    uint16_t samples;
    uint16_t padding;
    uint32_t size;
    bsdl_audio_cb callback;
    void *userdata;
} bsdl_audio_spec;

/* SDL_Event is 56 bytes in SDL2; leave headroom */
typedef union {
    uint32_t type;
    uint64_t align;
    uint8_t padding[128];
} bsdl_event;

typedef struct {
    int (*Init)(uint32_t flags);
    void (*Quit)(void);
    const char *(*GetError)(void);
    void (*GetVersion)(bsdl_version *ver);
    const char *(*GetCurrentVideoDriver)(void);
    const char *(*GetCurrentAudioDriver)(void);
    bsdl_window *(*CreateWindow)(const char *title, int x, int y, int w, int h, uint32_t flags);
    void (*DestroyWindow)(bsdl_window *w);
    int (*ShowCursor)(int toggle);
    int (*GL_SetAttribute)(int attr, int value);
    bsdl_glcontext (*GL_CreateContext)(bsdl_window *w);
    void (*GL_DeleteContext)(bsdl_glcontext c);
    void *(*GL_GetProcAddress)(const char *proc);
    void (*GL_SwapWindow)(bsdl_window *w);
    int (*GL_SetSwapInterval)(int interval);
    void (*GL_GetDrawableSize)(bsdl_window *w, int *wp, int *hp);
    int (*PollEvent)(bsdl_event *e);
    bsdl_audio_device (*OpenAudioDevice)(const char *device, int iscapture, const bsdl_audio_spec *desired,
                                         bsdl_audio_spec *obtained, int allowed_changes);
    void (*PauseAudioDevice)(bsdl_audio_device dev, int pause_on);
    void (*CloseAudioDevice)(bsdl_audio_device dev);
} bsdl_api;

extern bsdl_api sdl;

/* dlopen libSDL2 and resolve the table; 0 on success, else message in err */
int sdl_load(char *err, size_t n);

#endif
