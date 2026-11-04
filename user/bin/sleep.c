#include <stdlib.h>
#include <time.h>
int main(int argc, char **argv) {
    double s = argc > 1 ? strtod(argv[1], NULL) : 1;
    struct timespec ts = { (time_t)s, (long)((s - (time_t)s) * 1e9) };
    while (nanosleep(&ts, &ts) != 0) {}
    return 0;
}
