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
#define BSDL_INIT_JOYSTICK 0x00000200u
#define BSDL_INIT_GAMECONTROLLER 0x00002000u

/* SDL_GameControllerButton and SDL_GameControllerAxis */
#define BSDL_CONTROLLER_BUTTON_A 0
#define BSDL_CONTROLLER_BUTTON_B 1
#define BSDL_CONTROLLER_BUTTON_X 2
#define BSDL_CONTROLLER_BUTTON_Y 3
#define BSDL_CONTROLLER_BUTTON_BACK 4
#define BSDL_CONTROLLER_BUTTON_GUIDE 5
#define BSDL_CONTROLLER_BUTTON_START 6
#define BSDL_CONTROLLER_BUTTON_LEFTSTICK 7
#define BSDL_CONTROLLER_BUTTON_RIGHTSTICK 8
#define BSDL_CONTROLLER_BUTTON_LEFTSHOULDER 9
#define BSDL_CONTROLLER_BUTTON_RIGHTSHOULDER 10
#define BSDL_CONTROLLER_BUTTON_DPAD_UP 11
#define BSDL_CONTROLLER_BUTTON_DPAD_DOWN 12
#define BSDL_CONTROLLER_BUTTON_DPAD_LEFT 13
#define BSDL_CONTROLLER_BUTTON_DPAD_RIGHT 14
#define BSDL_CONTROLLER_AXIS_LEFTX 0
#define BSDL_CONTROLLER_AXIS_LEFTY 1
#define BSDL_CONTROLLER_AXIS_RIGHTX 2
#define BSDL_CONTROLLER_AXIS_RIGHTY 3
#define BSDL_CONTROLLER_AXIS_TRIGGERLEFT 4
#define BSDL_CONTROLLER_AXIS_TRIGGERRIGHT 5

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
typedef struct bsdl_gamecontroller bsdl_gamecontroller;
typedef struct bsdl_joystick bsdl_joystick;
typedef struct bsdl_rwops bsdl_rwops;
typedef struct {
    uint8_t data[16];
} bsdl_joystick_guid;
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
    /* Game controllers (runner/pad_sdl.cpp). Optional: NULL when the library lacks them. */
    int (*InitSubSystem)(uint32_t flags);
    int (*NumJoysticks)(void);
    int (*IsGameController)(int index);
    const char *(*JoystickNameForIndex)(int index);
    bsdl_joystick_guid (*JoystickGetDeviceGUID)(int index);
    void (*JoystickGetGUIDString)(bsdl_joystick_guid guid, char *out, int n);
    bsdl_gamecontroller *(*GameControllerOpen)(int index);
    const char *(*GameControllerName)(bsdl_gamecontroller *c);
    char *(*GameControllerMapping)(bsdl_gamecontroller *c);
    void (*Free)(void *p);
    int (*GameControllerGetAttached)(bsdl_gamecontroller *c);
    void (*GameControllerClose)(bsdl_gamecontroller *c);
    uint8_t (*GameControllerGetButton)(bsdl_gamecontroller *c, int button);
    int16_t (*GameControllerGetAxis)(bsdl_gamecontroller *c, int axis);
    bsdl_rwops *(*RWFromFile)(const char *file, const char *mode);
    int (*GameControllerAddMappingsFromRW)(bsdl_rwops *rw, int freerw);
} bsdl_api;

extern bsdl_api sdl;

/* dlopen libSDL2 and resolve the table; 0 on success, else message in err */
int sdl_load(char *err, size_t n);

#endif
