/* The Browser as a user program: the same code that ran in the kernel, on libui. */
#include "ui.h"

void browser_main(void *arg);

int main(int argc, char **argv) {
    ui_init();
    browser_main(argc > 1 ? argv[1] : NULL);
    return 0;
}
