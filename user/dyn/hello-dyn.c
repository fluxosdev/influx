/* A dynamically linked program: the kernel starts musl's dynamic linker (PT_INTERP), which maps libc.so. */
#include <stdio.h>
#include <dlfcn.h>
#include <link.h>

static int count(struct dl_phdr_info *info, size_t size, void *n) { ++*(int *)n; return 0; }

int main(int argc, char **argv) {
    int objects = 0;
    dl_iterate_phdr(count, &objects);
    printf("hello from a dynamically linked program (%d loaded objects)\n", objects);
    return objects >= 2 ? 0 : 1;
}
