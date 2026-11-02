#pragma once
#include <stdlib.h>
#include <string.h>
#define GL_MALLOC(n) malloc(n)
#define GL_FREE(p) free(p)
#define GL_MEMSET(p, v, n) memset(p, v, n)
#define GL_MEMCPY(d, s, n) memcpy(d, s, n)
#define GL_STRLEN(s) strlen(s)
