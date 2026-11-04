#include <stdio.h>
#include <unistd.h>
#include <sys/utsname.h>

int main(int argc, char **argv) {
    struct utsname u; uname(&u);
    printf("Hello from user space! This is a ring-3 program running on %s %s.\n", u.sysname, u.release);
    printf("My PID is %d. I was started with %d argument%s", getpid(), argc - 1, argc == 2 ? "" : "s");
    if (argc > 1) { printf(":"); for (int i = 1; i < argc; i++) printf(" %s", argv[i]); }
    printf("\n");
    return 0;
}
