/* ls: lists a directory from user mode (opendir/readdir/stat, through musl). */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : ".";
    DIR *d = opendir(path);
    if (!d) { fprintf(stderr, "ls: %s: %s\n", path, strerror(errno)); return 1; }
    printf("%-30s %10s  %s\n", "NAME", "SIZE", "TYPE");
    int total = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        char full[4096]; struct stat st;
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        long size = stat(full, &st) == 0 ? (long)st.st_size : 0;
        const char *type = e->d_type == DT_DIR ? "dir" : e->d_type == DT_REG ? "file" : "device";
        printf("%-30s %10ld  %s\n", e->d_name, size, type);
        total++;
    }
    closedir(d);
    printf("%d entries (listed by a user-mode program)\n", total);
    return 0;
}
