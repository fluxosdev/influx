#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <influx.h>

static int dump(int fd) {
    char buf[4096]; long n;
    while ((n = read(fd, buf, sizeof buf)) > 0) write(1, buf, n);
    return n < 0 ? 1 : 0;
}
int main(int argc, char **argv) {
    if (argc < 2) return dump(0);
    int status = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { fprintf(2, "cat: %s: ", argv[i]); perror(""); status = 1; continue; }
        status |= dump(fd);
        close(fd);
    }
    return status;
}
