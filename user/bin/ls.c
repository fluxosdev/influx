#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <influx.h>

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : ".";
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) { fprintf(2, "ls: %s: %s\n", path, strerror(errno)); return 1; }
    struct dirent d; int i = 0, total = 0;
    printf("%-30s %10s  %s\n", "NAME", "SIZE", "TYPE");
    while (readdir(fd, i++, &d) == 1) {
        printf("%-30s %10lu  %s\n", d.name, (unsigned long)d.size, d.type == VN_DIR ? "dir" : d.type == VN_FILE ? "file" : "device");
        total++;
    }
    close(fd);
    printf("%d entries (listed by a user-mode program)\n", total);
    return 0;
}
