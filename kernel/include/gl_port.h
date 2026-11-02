#pragma once
#include "mm.h"
#include "string.h"
#define GL_MALLOC(n) kmalloc(n)
#define GL_FREE(p) kfree(p)
#define GL_MEMSET(p, v, n) memset(p, v, n)
#define GL_MEMCPY(d, s, n) memcpy(d, s, n)
#define GL_STRLEN(s) strlen(s)
