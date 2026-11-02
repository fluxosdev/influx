#include <stdlib.h>
#include <unistd.h>
int main(int argc, char **argv) { sleep_ms(argc > 1 ? (unsigned)(atoi(argv[1]) * 1000) : 1000); return 0; }
