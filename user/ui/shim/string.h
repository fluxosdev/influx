/* user-space build of kernel sources: the kernel's "string.h" is musl's string, ctype and stdio */
#pragma once
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
static inline void memset32(uint32_t *d, uint32_t v, size_t n) { while (n--) *d++ = v; }
