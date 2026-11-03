#pragma once
/* what atom.c needs from its environment (a host build provides its own copy) */
#include "mm.h"
#include "string.h"
#define ATOM_MALLOC(n) kmalloc(n)
#define ATOM_FREE(p) kfree(p)
#define ATOM_MEMSET(p, v, n) memset(p, v, n)
#define ATOM_MEMCPY(d, s, n) memcpy(d, s, n)
#define ATOM_LOG(...) kprintf("atom: " __VA_ARGS__)
