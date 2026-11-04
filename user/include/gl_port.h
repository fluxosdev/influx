#pragma once
#include <stdlib.h>
#include <string.h>
#define GL_MALLOC(n) malloc(n)
#define GL_FREE(p) free(p)
#define GL_MEMSET(p, v, n) memset(p, v, n)
#define GL_MEMCPY(d, s, n) memcpy(d, s, n)
#define GL_STRLEN(s) strlen(s)
/* the current context is per thread, as OpenGL defines it (gl.c is the only includer) */
static __thread void *gl_port_cur;
#define GL_GET_CURRENT() gl_port_cur
#define GL_SET_CURRENT(c) (gl_port_cur = (c), 1)
#define GL_FORGET_CONTEXT(c) do { if (gl_port_cur == (c)) gl_port_cur = 0; } while (0)
