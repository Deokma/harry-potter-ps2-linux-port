#define _POSIX_C_SOURCE 200809L
#include "gl_dyn.h"

#include <stdio.h>
#include <string.h>

#define GLFN(type, name) type name;
#include "gl_funcs.h"
#undef GLFN
PFNGLGETQUERYOBJECTUI64VEXTPROC glGetQueryObjectui64vEXT;

int gl_load(void *(*getproc)(const char *), char *err, size_t n)
{
    void *p;
#define GLFN(type, name)                                     \
    p = getproc(#name);                                      \
    if (!p) {                                                \
        snprintf(err, n, "GL entry point %s missing", #name); \
        return -1;                                           \
    }                                                        \
    memcpy(&name, &p, sizeof p);
#include "gl_funcs.h"
#undef GLFN
    if (gl_has_ext("GL_EXT_disjoint_timer_query")) {
        p = getproc("glGetQueryObjectui64vEXT");
        memcpy(&glGetQueryObjectui64vEXT, &p, sizeof p);
    }
    return 0;
}

int gl_has_ext(const char *name)
{
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; i++) {
        const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (e && !strcmp(e, name))
            return 1;
    }
    return 0;
}
