/* Deliberately faults so you can watch the kernel terminate a misbehaving process. */
#include <stdio.h>
int main(int argc, char **argv) {
    printf("About to dereference a null pointer in user space...\n");
    volatile int *p = (volatile int *)0;
    *p = 42;
    printf("unreachable\n");
    return 0;
}
