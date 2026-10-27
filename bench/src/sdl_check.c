/* Native builds only: sdl_min.h must agree with the real SDL2 headers.
   Compiled, never linked; a mismatch fails the build. */
#include <SDL.h>
#include <stddef.h>

#include "sdl_min.h"

#define SAME(a, b) _Static_assert((a) == (b), #a " != " #b)

SAME(BSDL_INIT_AUDIO, SDL_INIT_AUDIO);
SAME(BSDL_INIT_VIDEO, SDL_INIT_VIDEO);
SAME(BSDL_INIT_EVENTS, SDL_INIT_EVENTS);
SAME(BSDL_WINDOW_FULLSCREEN, SDL_WINDOW_FULLSCREEN);
SAME(BSDL_WINDOW_OPENGL, SDL_WINDOW_OPENGL);
SAME(BSDL_WINDOW_FULLSCREEN_DESKTOP, SDL_WINDOW_FULLSCREEN_DESKTOP);
SAME(BSDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED);
SAME(BSDL_GL_RED_SIZE, SDL_GL_RED_SIZE);
SAME(BSDL_GL_GREEN_SIZE, SDL_GL_GREEN_SIZE);
SAME(BSDL_GL_BLUE_SIZE, SDL_GL_BLUE_SIZE);
SAME(BSDL_GL_ALPHA_SIZE, SDL_GL_ALPHA_SIZE);
SAME(BSDL_GL_DOUBLEBUFFER, SDL_GL_DOUBLEBUFFER);
SAME(BSDL_GL_DEPTH_SIZE, SDL_GL_DEPTH_SIZE);
SAME(BSDL_GL_STENCIL_SIZE, SDL_GL_STENCIL_SIZE);
SAME(BSDL_GL_CONTEXT_MAJOR_VERSION, SDL_GL_CONTEXT_MAJOR_VERSION);
SAME(BSDL_GL_CONTEXT_MINOR_VERSION, SDL_GL_CONTEXT_MINOR_VERSION);
SAME(BSDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_MASK);
SAME(BSDL_GL_CONTEXT_PROFILE_ES, SDL_GL_CONTEXT_PROFILE_ES);
SAME(BSDL_QUIT, SDL_QUIT);
SAME(BSDL_KEYDOWN, SDL_KEYDOWN);
SAME(BSDL_AUDIO_S16LSB, AUDIO_S16LSB);

SAME(sizeof(bsdl_version), sizeof(SDL_version));
SAME(sizeof(bsdl_audio_spec), sizeof(SDL_AudioSpec));
SAME(offsetof(bsdl_audio_spec, freq), offsetof(SDL_AudioSpec, freq));
SAME(offsetof(bsdl_audio_spec, format), offsetof(SDL_AudioSpec, format));
SAME(offsetof(bsdl_audio_spec, channels), offsetof(SDL_AudioSpec, channels));
SAME(offsetof(bsdl_audio_spec, samples), offsetof(SDL_AudioSpec, samples));
SAME(offsetof(bsdl_audio_spec, size), offsetof(SDL_AudioSpec, size));
SAME(offsetof(bsdl_audio_spec, callback), offsetof(SDL_AudioSpec, callback));
SAME(offsetof(bsdl_audio_spec, userdata), offsetof(SDL_AudioSpec, userdata));
_Static_assert(sizeof(bsdl_event) >= sizeof(SDL_Event), "bsdl_event too small");
SAME(offsetof(SDL_Event, type), 0);
SAME(sizeof(bsdl_audio_device), sizeof(SDL_AudioDeviceID));
