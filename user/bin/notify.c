/* notify: show a notification banner on the desktop: notify TITLE [MESSAGE] */
#include <stdio.h>
#include <influx.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: notify TITLE [MESSAGE]\n"); return 2; }
    if (influx_notify(argv[1], argc > 2 ? argv[2] : "") < 0) { perror("notify"); return 1; }
    return 0;
}
