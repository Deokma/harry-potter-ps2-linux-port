/* GLES 3.0 through function pointers resolved at run time. The Khronos
   headers supply types, enums and PFN typedefs; with prototypes disabled,
   each glXxx below is a pointer variable instead (hidden, so it never
   interposes on the driver's own symbols). */
#ifndef GL_DYN_H
#define GL_DYN_H

#define GL_GLES_PROTOTYPES 0
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <stddef.h>

#define GLFN(type, name) extern __attribute__((visibility("hidden"))) type name;
#include "gl_funcs.h"
#undef GLFN

/* optional: EXT_disjoint_timer_query (NULL if absent) */
extern __attribute__((visibility("hidden"))) PFNGLGETQUERYOBJECTUI64VEXTPROC glGetQueryObjectui64vEXT;

int gl_load(void *(*getproc)(const char *), char *err, size_t n);
int gl_has_ext(const char *name);

#endif
