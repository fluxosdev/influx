/*
 * systest: checks the kernel's system call interface from user mode, through
 * musl. Prints one PASS/FAIL line per check and exits with the number of
 * failures. `systest net` adds DNS and TCP checks (they need a network).
 * The boot option `selftest` runs it at startup and powers off (CI).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <dirent.h>

static int failures, checks;
#define CHECK(name, cond) do { checks++; if (cond) printf("PASS %s\n", name); else { failures++; printf("FAIL %s (errno %d: %s)\n", name, errno, strerror(errno)); } fflush(stdout); } while (0)

/* ---- memory ---- */
static void test_memory(void) {
    char *big = malloc(64 << 20);   /* mmap'd by musl, demand-paged by the kernel */
    bool ok = big != NULL;
    if (ok) { for (size_t i = 0; i < (64u << 20); i += 4096) big[i] = (char)i; for (size_t i = 0; i < (64u << 20); i += 4096) if (big[i] != (char)i) ok = false; }
    free(big);
    CHECK("malloc 64 MiB and touch every page", ok);
    void *p = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK("mmap anonymous", p != MAP_FAILED && ((char *)p)[12345] == 0);
    if (p != MAP_FAILED) CHECK("munmap", munmap(p, 1 << 20) == 0);
    void *fixed = mmap((void *)0x300000000000, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    CHECK("mmap MAP_FIXED", fixed == (void *)0x300000000000);
    /* musl's sbrk only answers sbrk(0): the system call itself */
    char *cur = (char *)syscall(SYS_brk, 0), *nb = (char *)syscall(SYS_brk, cur + 65536);
    bool brk_ok = nb == cur + 65536;
    if (brk_ok) { cur[0] = 1; cur[65535] = 2; brk_ok = (char *)syscall(SYS_brk, cur) == cur; }
    CHECK("brk grows, is usable and shrinks", brk_ok);
}

/* ---- faults and signals ---- */
static sigjmp_buf jb;
static volatile void *fault_addr;
static void on_segv(int sig, siginfo_t *si, void *uc) { fault_addr = si->si_addr; siglongjmp(jb, 1); }
static volatile int got_usr1, got_alrm;
static void on_usr1(int s) { got_usr1++; }

static void test_signals(void) {
    struct sigaction sa = { 0 };
    sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    volatile int recovered = 0;
    if (!sigsetjmp(jb, 1)) { *(volatile int *)0x1234 = 1; } else recovered = 1;
    CHECK("SIGSEGV handler with si_addr, siglongjmp out", recovered && fault_addr == (void *)0x1234);
    char *ro = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ro[0] = 1;
    mprotect(ro, 4096, PROT_READ);
    recovered = 0;
    if (!sigsetjmp(jb, 1)) { ro[1] = 2; } else recovered = 1;
    CHECK("mprotect read-only page faults on write", recovered && ro[1] == 0);
    signal(SIGSEGV, SIG_DFL);

    signal(SIGUSR1, on_usr1);
    raise(SIGUSR1);
    CHECK("raise + handler", got_usr1 == 1);
    sigset_t m; sigemptyset(&m); sigaddset(&m, SIGUSR1);
    sigprocmask(SIG_BLOCK, &m, NULL);
    kill(getpid(), SIGUSR1);
    int before = got_usr1;
    sigprocmask(SIG_UNBLOCK, &m, NULL);
    CHECK("blocked signal delivered on unblock", before == 1 && got_usr1 == 2);

    /* a signal interrupting a blocking read: EINTR without SA_RESTART */
    int pfd[2]; pipe(pfd);
    struct sigaction ua = { 0 }; ua.sa_handler = on_usr1;
    sigaction(SIGUSR1, &ua, NULL);
    pid_t parent = getpid();
    pid_t c = fork();
    if (c == 0) { struct timespec ts = { 0, 100000000 }; nanosleep(&ts, NULL); kill(parent, SIGUSR1); _exit(0); }
    char b; errno = 0;
    ssize_t r = read(pfd[0], &b, 1);
    CHECK("blocking read interrupted: EINTR", r == -1 && errno == EINTR);
    waitpid(c, NULL, 0);
    close(pfd[0]); close(pfd[1]);

    c = fork();
    if (c == 0) { for (;;) pause(); }
    kill(c, SIGTERM);
    int st = 0; waitpid(c, &st, 0);
    CHECK("kill child: WIFSIGNALED(SIGTERM)", WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);
}

/* ---- processes ---- */
static void test_processes(void) {
    int shared = 41;
    pid_t c = fork();
    if (c == 0) { shared++; _exit(shared == 42 ? 7 : 1); }
    int st = 0;
    CHECK("fork + waitpid", waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 7);
    CHECK("copy-on-write: parent's copy unchanged", shared == 41);

    int pfd[2]; pipe(pfd);
    c = fork();
    if (c == 0) { dup2(pfd[1], 1); close(pfd[0]); close(pfd[1]); execl("/bin/echo", "echo", "exec", "works", (char *)NULL); _exit(127); }
    close(pfd[1]);
    char buf[64] = { 0 }; size_t n = 0; ssize_t r;
    while ((r = read(pfd[0], buf + n, sizeof buf - 1 - n)) > 0) n += (size_t)r;
    close(pfd[0]);
    waitpid(c, &st, 0);
    CHECK("fork + execve + pipe", !strcmp(buf, "exec works\n") && WIFEXITED(st) && WEXITSTATUS(st) == 0);

    pipe(pfd);
    c = fork();
    if (c == 0) { dup2(pfd[1], 1); close(pfd[0]); close(pfd[1]); execl("/bin/hello-dyn", "hello-dyn", (char *)NULL); _exit(127); }
    close(pfd[1]);
    memset(buf, 0, sizeof buf); n = 0;
    while ((r = read(pfd[0], buf + n, sizeof buf - 1 - n)) > 0) n += (size_t)r;
    close(pfd[0]);
    waitpid(c, &st, 0);
    CHECK("dynamically linked program (PT_INTERP, musl ld.so)", !strncmp(buf, "hello from a dynamically linked program", 39) && WIFEXITED(st) && WEXITSTATUS(st) == 0);

    CHECK("waitpid with no children: ECHILD", waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);
    CHECK("execve of a missing file: ENOENT", execl("/bin/nonexistent", "x", (char *)NULL) == -1 && errno == ENOENT);

    pid_t sp;
    extern char **environ;
    char *argv[] = { "echo", "spawned", NULL };
    int dn = open("/dev/null", O_WRONLY);
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    if (dn >= 0) posix_spawn_file_actions_adddup2(&fa, dn, 1);
    int e = posix_spawn(&sp, "/bin/echo", &fa, NULL, argv, environ);
    CHECK("posix_spawn", e == 0 && waitpid(sp, &st, 0) == sp && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    if (dn >= 0) close(dn);
}

/* ---- threads ---- */
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static long counter;
static int ready;
static __thread int tls_value;
static void *worker(void *arg) {
    tls_value = (int)(long)arg;
    for (int i = 0; i < 100000; i++) { pthread_mutex_lock(&mtx); counter++; pthread_mutex_unlock(&mtx); }
    return (void *)(long)(tls_value * 2);
}
static void *waiter(void *arg) {
    pthread_mutex_lock(&mtx);
    while (!ready) pthread_cond_wait(&cv, &mtx);
    pthread_mutex_unlock(&mtx);
    return (void *)1;
}
static void test_threads(void) {
    pthread_t t[4];
    tls_value = 99;
    bool ok = true;
    for (long i = 0; i < 4; i++) ok &= pthread_create(&t[i], NULL, worker, (void *)(i + 1)) == 0;
    long sum = 0;
    for (int i = 0; i < 4; i++) { void *r; ok &= pthread_join(t[i], &r) == 0; sum += (long)r; }
    CHECK("4 threads, contended mutex", ok && counter == 400000);
    CHECK("thread-local storage per thread", sum == 2 * (1 + 2 + 3 + 4) && tls_value == 99);
    pthread_t w[3];
    for (int i = 0; i < 3; i++) pthread_create(&w[i], NULL, waiter, NULL);
    struct timespec ts = { 0, 50000000 }; nanosleep(&ts, NULL);
    pthread_mutex_lock(&mtx); ready = 1; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mtx);
    long woke = 0;
    for (int i = 0; i < 3; i++) { void *r; pthread_join(w[i], &r); woke += (long)r; }
    CHECK("condition variable broadcast", woke == 3);
}

/* ---- files and descriptors ---- */
static void test_files(void) {
    int fd = open("/tmp/systest.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK("open O_CREAT", fd >= 0);
    CHECK("write", write(fd, "hello world\n", 12) == 12);
    CHECK("lseek + read", lseek(fd, 6, SEEK_SET) == 6 && ({ char b[8] = { 0 }; read(fd, b, 5) == 5 && !memcmp(b, "world", 5); }));
    char pb[6] = { 0 };
    CHECK("pread", pread(fd, pb, 5, 0) == 5 && !strcmp(pb, "hello"));
    struct stat st;
    CHECK("fstat size and type", fstat(fd, &st) == 0 && st.st_size == 12 && S_ISREG(st.st_mode));
    char *map = mmap(NULL, 12, PROT_READ, MAP_PRIVATE, fd, 0);
    CHECK("mmap a file", map != MAP_FAILED && !memcmp(map, "hello world", 11));
    close(fd);
    CHECK("O_EXCL on an existing file: EEXIST", open("/tmp/systest.txt", O_CREAT | O_EXCL | O_WRONLY, 0644) == -1 && errno == EEXIST);
    FILE *f = fopen("/tmp/systest.txt", "a"); fprintf(f, "%s %.3f\n", "pi", 3.14159); fclose(f);
    char line[64] = { 0 }; f = fopen("/tmp/systest.txt", "r"); fgets(line, sizeof line, f); fgets(line, sizeof line, f); fclose(f);
    CHECK("stdio fopen/fprintf(%f)/fgets", !strcmp(line, "pi 3.142\n"));
    CHECK("rename", rename("/tmp/systest.txt", "/tmp/systest2.txt") == 0 && access("/tmp/systest2.txt", F_OK) == 0);
    CHECK("unlink", unlink("/tmp/systest2.txt") == 0 && access("/tmp/systest2.txt", F_OK) == -1 && errno == ENOENT);
    CHECK("mkdir + rmdir", mkdir("/tmp/systest.d", 0755) == 0 && rmdir("/tmp/systest.d") == 0);
    DIR *d = opendir("/bin"); int n = 0; bool seen = false; struct dirent *de;
    while (d && (de = readdir(d))) { n++; if (!strcmp(de->d_name, "systest")) seen = true; }
    if (d) closedir(d);
    CHECK("opendir/readdir", n > 3 && seen);
    char cwd[256];
    CHECK("chdir + getcwd", chdir("/tmp") == 0 && getcwd(cwd, sizeof cwd) && !strcmp(cwd, "/tmp"));
    int dfd = dup(1);
    CHECK("dup + fcntl FD_CLOEXEC", dfd > 2 && fcntl(dfd, F_SETFD, FD_CLOEXEC) == 0 && fcntl(dfd, F_GETFD) == FD_CLOEXEC);
    close(dfd);
}

/* ---- symbolic links ---- */
static void test_symlinks(void) {
    mkdir("/tmp/sl", 0755);
    FILE *f = fopen("/tmp/sl/target.txt", "w"); fputs("linked", f); fclose(f);
    CHECK("symlink", symlink("target.txt", "/tmp/sl/rel") == 0 && symlink("/tmp/sl", "/tmp/sl-abs") == 0);
    char buf[64] = { 0 };
    CHECK("readlink", readlink("/tmp/sl/rel", buf, sizeof buf) == 10 && !strncmp(buf, "target.txt", 10));
    struct stat st;
    CHECK("lstat sees the link, stat the target", lstat("/tmp/sl/rel", &st) == 0 && S_ISLNK(st.st_mode) && stat("/tmp/sl/rel", &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 6);
    memset(buf, 0, sizeof buf);
    f = fopen("/tmp/sl-abs/rel", "r");   /* through two links: a directory one, then a relative one */
    CHECK("open through a directory link and a relative link", f && fgets(buf, sizeof buf, f) && !strcmp(buf, "linked"));
    if (f) fclose(f);
    symlink("loop2", "/tmp/sl/loop1"); symlink("loop1", "/tmp/sl/loop2");
    CHECK("a link loop fails instead of hanging", open("/tmp/sl/loop1", O_RDONLY) == -1);
    CHECK("rename a link", rename("/tmp/sl/rel", "/tmp/sl/rel2") == 0 && readlink("/tmp/sl/rel2", buf, sizeof buf) == 10);
    CHECK("unlink removes the link, not the target", unlink("/tmp/sl/rel2") == 0 && access("/tmp/sl/target.txt", F_OK) == 0);
    unlink("/tmp/sl/loop1"); unlink("/tmp/sl/loop2"); unlink("/tmp/sl/target.txt"); unlink("/tmp/sl-abs"); rmdir("/tmp/sl");
}

/* runs argv with stdout into buf (NUL-terminated); returns the exit status, or -1 */
static int run_capture(char *const argv[], char *buf, size_t size) {
    int pfd[2]; if (pipe(pfd) < 0) return -1;
    pid_t c = fork();
    if (c == 0) { dup2(pfd[1], 1); close(pfd[0]); close(pfd[1]); execv(argv[0], argv); _exit(126 + (errno == ENOENT)); }
    close(pfd[1]);
    size_t n = 0; ssize_t r;
    memset(buf, 0, size);
    while ((r = read(pfd[0], buf + n, size - 1 - n)) > 0) n += (size_t)r;
    close(pfd[0]);
    int st;
    if (waitpid(c, &st, 0) != c || !WIFEXITED(st)) return -1;
    return WEXITSTATUS(st);
}
static bool write_file(const char *path, const char *text, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return false;
    bool ok = write(fd, text, strlen(text)) == (ssize_t)strlen(text);
    return close(fd) == 0 && ok;
}
static bool read_file(const char *path, char *buf, size_t size) {
    int fd = open(path, O_RDONLY); if (fd < 0) return false;
    memset(buf, 0, size);
    ssize_t r = read(fd, buf, size - 1);
    close(fd);
    return r >= 0;
}

/* ---- program loading: #! scripts and the execute permission ---- */
static void test_exec(void) {
    char out[256];
    CHECK("#! script: interpreter gets [argument,] script path, arguments", write_file("/tmp/st-script", "#!/bin/echo from-script\nignored\n", 0755) &&
          run_capture((char *[]){ "/tmp/st-script", "a1", "a2", NULL }, out, sizeof out) == 0 && !strcmp(out, "from-script /tmp/st-script a1 a2\n"));
    CHECK("#! script without an argument", write_file("/tmp/st-script2", "#!/bin/echo\n", 0755) &&
          run_capture((char *[]){ "/tmp/st-script2", "x", NULL }, out, sizeof out) == 0 && !strcmp(out, "/tmp/st-script2 x\n"));
    CHECK("#! script whose interpreter is a script", write_file("/tmp/st-script3", "#!/tmp/st-script2 nested\n", 0755) &&
          run_capture((char *[]){ "/tmp/st-script3", NULL }, out, sizeof out) == 0 && !strcmp(out, "/tmp/st-script2 nested /tmp/st-script3\n"));
    CHECK("execve without an execute bit: EACCES", write_file("/tmp/st-noexec", "#!/bin/echo\n", 0644) &&
          execl("/tmp/st-noexec", "x", (char *)NULL) == -1 && errno == EACCES);
    CHECK("execve of a script with a missing interpreter: ENOENT", write_file("/tmp/st-badinterp", "#!/nonexistent/sh\n", 0755) &&
          execl("/tmp/st-badinterp", "x", (char *)NULL) == -1 && errno == ENOENT);
    CHECK("execve of a directory: EACCES", execl("/tmp", "x", (char *)NULL) == -1 && errno == EACCES);
    CHECK("access X_OK follows the mode", access("/tmp/st-script", X_OK) == 0 && access("/tmp/st-noexec", X_OK) == -1 && errno == EACCES);
    unlink("/tmp/st-script"); unlink("/tmp/st-script2"); unlink("/tmp/st-script3"); unlink("/tmp/st-noexec"); unlink("/tmp/st-badinterp");
}

/* ---- what make, ar, ld and install need from a filesystem; `systest posix DIR` runs it on DIR ---- */
static void test_posix(const char *dir) {
    char a[512], b[512], c[512], out[64];
    struct stat st, st2;
    mode_t old = umask(022);
    snprintf(a, sizeof a, "%s/px-mode", dir);
    int fd = open(a, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    CHECK("open(O_CREAT, 0777) under umask 022 gives 0755", fd >= 0 && fstat(fd, &st) == 0 && (st.st_mode & 07777) == 0755);
    if (fd >= 0) close(fd);
    CHECK("chmod", chmod(a, 0640) == 0 && stat(a, &st) == 0 && (st.st_mode & 07777) == 0640);
    fd = open(a, O_RDONLY);
    CHECK("fchmod", fd >= 0 && fchmod(fd, 0711) == 0 && stat(a, &st) == 0 && (st.st_mode & 07777) == 0711);
    if (fd >= 0) close(fd);
    CHECK("fchmodat", fchmodat(AT_FDCWD, a, 0600, 0) == 0 && stat(a, &st) == 0 && (st.st_mode & 07777) == 0600);
    snprintf(b, sizeof b, "%s/px-dir", dir);
    CHECK("mkdir mode under umask", mkdir(b, 0777) == 0 && stat(b, &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & 07777) == 0755);
    rmdir(b);

    /* timestamps, to the nanosecond */
    struct timespec ts[2] = { { 0, UTIME_OMIT }, { 1000000000, 123456789 } };
    CHECK("utimensat sets mtime with nanoseconds", utimensat(AT_FDCWD, a, ts, 0) == 0 && stat(a, &st) == 0 && st.st_mtim.tv_sec == 1000000000 && st.st_mtim.tv_nsec == 123456789);
    ts[1].tv_nsec = UTIME_OMIT;
    CHECK("utimensat UTIME_OMIT leaves mtime", utimensat(AT_FDCWD, a, ts, 0) == 0 && stat(a, &st) == 0 && st.st_mtim.tv_sec == 1000000000);
    fd = open(a, O_WRONLY);
    struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
    CHECK("futimens(NULL) sets mtime to now", fd >= 0 && futimens(fd, NULL) == 0 && fstat(fd, &st) == 0 && st.st_mtim.tv_sec >= now.tv_sec - 1 && st.st_mtim.tv_sec <= now.tv_sec + 1);
    if (fd >= 0) close(fd);
    struct timeval tv[2] = { { 2000000000, 0 }, { 2000000000, 500000 } };
    CHECK("utimes (microseconds)", utimes(a, tv) == 0 && stat(a, &st) == 0 && st.st_mtim.tv_sec == 2000000000 && st.st_mtim.tv_nsec == 500000000);
    write_file(a, "x", 0644);
    clock_gettime(CLOCK_REALTIME, &now);
    CHECK("a write updates mtime", stat(a, &st) == 0 && st.st_mtim.tv_sec >= now.tv_sec - 2 && st.st_mtim.tv_sec <= now.tv_sec + 1);
    struct timespec t1, t2;
    write_file(a, "1", 0644); stat(a, &st); t1 = st.st_mtim;
    nanosleep(&(struct timespec){ 0, 30000000 }, NULL);
    write_file(a, "2", 0644); stat(a, &st); t2 = st.st_mtim;
    CHECK("writes 30 ms apart get different mtimes", t2.tv_sec > t1.tv_sec || (t2.tv_sec == t1.tv_sec && t2.tv_nsec > t1.tv_nsec));

    /* hard links */
    snprintf(b, sizeof b, "%s/px-link", dir);
    write_file(a, "linked data", 0644);
    CHECK("link", link(a, b) == 0 && stat(a, &st) == 0 && stat(b, &st2) == 0 && st.st_ino == st2.st_ino && st.st_nlink == 2);
    CHECK("link onto an existing name: EEXIST", link(a, b) == -1 && errno == EEXIST);
    write_file(b, "via the link", 0644);
    CHECK("both names see one file", read_file(a, out, sizeof out) && !strcmp(out, "via the link"));
    CHECK("unlink one name, the other keeps the data", unlink(a) == 0 && stat(b, &st) == 0 && st.st_nlink == 1 && read_file(b, out, sizeof out) && !strcmp(out, "via the link"));
    snprintf(c, sizeof c, "%s/px-ldir", dir); mkdir(c, 0755);
    snprintf(a, sizeof a, "%s/px-ldir/second", dir);
    CHECK("link into another directory", link(b, a) == 0 && stat(b, &st) == 0 && st.st_nlink == 2);
    snprintf(a, sizeof a, "%s/px-ldir/third", dir);
    snprintf(out, sizeof out, "%s", "x");
    CHECK("two links in one directory", link(b, a) == 0 && stat(b, &st) == 0 && st.st_nlink == 3);
    unlink(a); snprintf(a, sizeof a, "%s/px-ldir/second", dir); unlink(a);
    CHECK("link counts drop with unlink", stat(b, &st) == 0 && st.st_nlink == 1);
    CHECK("link to a directory: EPERM", link(c, a) == -1 && errno == EPERM);

    /* rename: replaces in one step; directories move with their contents */
    snprintf(a, sizeof a, "%s/px-old", dir);
    write_file(a, "new contents", 0644);
    write_file(b, "old contents", 0644);
    CHECK("rename over an existing file", rename(a, b) == 0 && access(a, F_OK) == -1 && read_file(b, out, sizeof out) && !strcmp(out, "new contents"));
    snprintf(a, sizeof a, "%s/px-ldir/file", dir); write_file(a, "in dir", 0644);
    snprintf(a, sizeof a, "%s/px-ldir2", dir);
    CHECK("rename a non-empty directory", rename(c, a) == 0 && ({ char q[600]; snprintf(q, sizeof q, "%s/file", a); read_file(q, out, sizeof out) && !strcmp(out, "in dir"); }));
    char q[600]; snprintf(q, sizeof q, "%s/inner", a);
    CHECK("rename a directory into itself: EINVAL", rename(a, q) == -1 && errno == EINVAL);
    CHECK("rename a file over a directory: EISDIR", rename(b, a) == -1 && errno == EISDIR);
    CHECK("rename a directory over a file: ENOTDIR", rename(a, b) == -1 && errno == ENOTDIR);
    snprintf(c, sizeof c, "%s/px-empty", dir); mkdir(c, 0755);
    CHECK("rename a directory over an empty one", rename(a, c) == 0 && access(a, F_OK) == -1);
    snprintf(q, sizeof q, "%s/file", c); unlink(q); rmdir(c);
    snprintf(a, sizeof a, "%s/px-nr", dir); write_file(a, "a", 0644);
    CHECK("renameat2 RENAME_NOREPLACE", syscall(SYS_renameat2, AT_FDCWD, a, AT_FDCWD, b, 1) == -1 && errno == EEXIST && access(a, F_OK) == 0);
    unlink(a); unlink(b);

    /* named pipes */
    snprintf(a, sizeof a, "%s/px-fifo", dir);
    if (mkfifo(a, 0600) == 0) {
        CHECK("mkfifo makes a FIFO", stat(a, &st) == 0 && S_ISFIFO(st.st_mode));
        pid_t ch = fork();
        if (ch == 0) { int w = open(a, O_WRONLY); _exit(w >= 0 && write(w, "through the fifo", 16) == 16 ? 0 : 1); }
        int r = open(a, O_RDONLY);
        memset(out, 0, sizeof out); size_t n = 0; ssize_t k;
        while (r >= 0 && (k = read(r, out + n, sizeof out - 1 - n)) > 0) n += (size_t)k;
        if (r >= 0) close(r);
        int status = -1; waitpid(ch, &status, 0);
        CHECK("a FIFO carries data between processes, EOF when the writer closes", !strcmp(out, "through the fifo") && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        int rw = open(a, O_RDWR | O_NONBLOCK);
        char ch1;
        CHECK("FIFO opened O_RDWR: no wait, empty read is EAGAIN", rw >= 0 && read(rw, &ch1, 1) == -1 && errno == EAGAIN && write(rw, "z", 1) == 1 && read(rw, &ch1, 1) == 1 && ch1 == 'z');
        if (rw >= 0) close(rw);
        CHECK("FIFO: O_NONBLOCK writer with no reader: ENXIO", open(a, O_WRONLY | O_NONBLOCK) == -1 && errno == ENXIO);
        unlink(a);
    } else CHECK("mkfifo", false);

    struct statfs sf;
    CHECK("statfs", statfs(dir, &sf) == 0 && sf.f_bsize > 0 && sf.f_blocks > 0);
    umask(old);
}

/* ---- interval timers ---- */
static volatile int alarms;
static void on_alarm(int s) { alarms++; }
static void test_timers(void) {
    signal(SIGALRM, on_alarm);
    alarms = 0;
    CHECK("alarm(1) delivers SIGALRM", alarm(1) == 0 && ({ for (int i = 0; i < 300 && !alarms; i++) nanosleep(&(struct timespec){ 0, 10000000 }, NULL); alarms == 1; }));
    CHECK("alarm(0) cancels and returns what was left", alarm(5) == 0 && alarm(0) >= 4);
    alarms = 0;
    struct itimerval it = { { 0, 30000 }, { 0, 30000 } }, cur;
    CHECK("setitimer ITIMER_REAL repeats", setitimer(ITIMER_REAL, &it, NULL) == 0 && ({ for (int i = 0; i < 300 && alarms < 3; i++) nanosleep(&(struct timespec){ 0, 10000000 }, NULL); alarms >= 3; }));
    CHECK("getitimer", getitimer(ITIMER_REAL, &cur) == 0 && cur.it_interval.tv_usec == 30000);
    memset(&it, 0, sizeof it);
    setitimer(ITIMER_REAL, &it, NULL);
    int seen = alarms;
    nanosleep(&(struct timespec){ 0, 100000000 }, NULL);
    CHECK("setitimer with zero disarms", alarms == seen);
    /* pselect's mask: SIGALRM blocked outside, let through while waiting (how make waits for SIGCHLD) */
    sigset_t blk, none; sigemptyset(&blk); sigaddset(&blk, SIGALRM); sigemptyset(&none);
    sigprocmask(SIG_BLOCK, &blk, NULL);
    alarms = 0;
    setitimer(ITIMER_REAL, &(struct itimerval){ { 0, 0 }, { 0, 50000 } }, NULL);
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    int pr = pselect(0, NULL, NULL, NULL, &(struct timespec){ 3, 0 }, &none);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long waited = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    CHECK("pselect: a signal its mask unblocks interrupts it (EINTR)", pr == -1 && errno == EINTR && alarms == 1 && waited < 2000);
    sigprocmask(SIG_UNBLOCK, &blk, NULL);
    signal(SIGALRM, SIG_DFL);
}

/* ---- the developer tools (DEVTOOLS=1 images): compilers, sh and make running on inFlux ---- */
static bool run_ok(char *const argv[], char *out, size_t size) { return run_capture(argv, out, size) == 0; }
static void test_devtools(void) {
    char out[512];
    mkdir("/tmp/dt", 0755);
    write_file("/tmp/dt/hello.c", "#include <stdio.h>\nint main(void) { printf(\"built on inFlux: %d\\n\", 6 * 7); return 0; }\n", 0644);
    CHECK("gcc compiles and links (dynamic)", run_ok((char *[]){ "/usr/bin/gcc", "-O2", "-o", "/tmp/dt/hello", "/tmp/dt/hello.c", NULL }, out, sizeof out));
    CHECK("the program gcc made runs", run_ok((char *[]){ "/tmp/dt/hello", NULL }, out, sizeof out) && !strcmp(out, "built on inFlux: 42\n"));
    CHECK("gcc -static", run_ok((char *[]){ "/usr/bin/gcc", "-static", "-o", "/tmp/dt/hello-s", "/tmp/dt/hello.c", NULL }, out, sizeof out) &&
          run_ok((char *[]){ "/tmp/dt/hello-s", NULL }, out, sizeof out) && !strcmp(out, "built on inFlux: 42\n"));
    write_file("/tmp/dt/hello.cc", "#include <iostream>\n#include <vector>\nint main() { std::vector<int> v{1, 2, 3}; int s = 0; for (int x : v) s += x; std::cout << \"c++ \" << s << std::endl; }\n", 0644);
    CHECK("g++ (libstdc++, iostream)", run_ok((char *[]){ "/usr/bin/g++", "-O2", "-o", "/tmp/dt/hellocc", "/tmp/dt/hello.cc", NULL }, out, sizeof out) &&
          run_ok((char *[]){ "/tmp/dt/hellocc", NULL }, out, sizeof out) && !strcmp(out, "c++ 6\n"));
    CHECK("/bin/sh (BusyBox ash): arithmetic, loops, command substitution",
          run_ok((char *[]){ "/bin/sh", "-c", "echo $((6*7)); for i in 1 2 3; do printf %s $i; done; echo; echo $(echo nested)", NULL }, out, sizeof out) && !strcmp(out, "42\n123\nnested\n"));
    write_file("/tmp/dt/script", "#!/bin/sh\nset -e\ncd /tmp/dt\nx=$(ls hello.c)\ntest -x hello && echo \"script ok $x $1\"\n", 0755);
    CHECK("a #!/bin/sh script", run_ok((char *[]){ "/tmp/dt/script", "arg", NULL }, out, sizeof out) && !strcmp(out, "script ok hello.c arg\n"));
    CHECK("sh pipelines and sed/grep/sort/wc",
          run_ok((char *[]){ "/bin/sh", "-c", "printf 'b\\na\\nc\\n' | sort | sed 's/^/x/' | grep -v xc | tr '\\n' ' ' && ls /usr/bin | wc -l | (read n; test $n -gt 300 && echo many)", NULL }, out, sizeof out) && !strcmp(out, "xa xb many\n"));
    /* make: a parallel build, then an incremental one that redoes only what changed */
    for (int i = 0; i < 6; i++) { char p[64], src[128]; snprintf(p, sizeof p, "/tmp/dt/m%d.c", i); snprintf(src, sizeof src, "int f%d(void) { return %d; }\n", i, i + 1); write_file(p, src, 0644); }
    write_file("/tmp/dt/main.c", "#include <stdio.h>\nint f0(void),f1(void),f2(void),f3(void),f4(void),f5(void);\nint main(void){printf(\"sum %d\\n\",f0()+f1()+f2()+f3()+f4()+f5());return 0;}\n", 0644);
    write_file("/tmp/dt/Makefile", "OBJ := main.o $(patsubst %.c,%.o,$(wildcard m*.c))\nprog: $(OBJ)\n\t$(CC) -o $@ $^\n%.o: %.c\n\t@echo build $<\n\t@$(CC) -O2 -c -o $@ $<\nclean:\n\trm -f prog *.o\n", 0644);
    CHECK("make -j4", run_ok((char *[]){ "/usr/bin/make", "-s", "-C", "/tmp/dt", "-j4", "CC=gcc", "prog", NULL }, out, sizeof out) &&
          run_ok((char *[]){ "/tmp/dt/prog", NULL }, out, sizeof out) && !strcmp(out, "sum 21\n"));
    nanosleep(&(struct timespec){ 0, 20000000 }, NULL);
    write_file("/tmp/dt/m3.c", "int f3(void) { return 100; }\n", 0644);
    CHECK("make rebuilds only what changed", run_ok((char *[]){ "/usr/bin/make", "-s", "-C", "/tmp/dt", "CC=gcc", "prog", NULL }, out, sizeof out) && !strcmp(out, "build m3.c\n") &&
          run_ok((char *[]){ "/tmp/dt/prog", NULL }, out, sizeof out) && !strcmp(out, "sum 117\n"));
    CHECK("make: nothing to do when up to date", run_ok((char *[]){ "/usr/bin/make", "-s", "-C", "/tmp/dt", "CC=gcc", "prog", NULL }, out, sizeof out) && !strcmp(out, ""));
    CHECK("ar + nm", run_ok((char *[]){ "/bin/sh", "-c", "cd /tmp/dt && ar rcs libm.a m0.o m1.o && nm libm.a | grep -c ' T f'", NULL }, out, sizeof out) && !strcmp(out, "2\n"));
}

/* ---- make test-persist: what one boot writes, the next must find (`systest persist write|check`) ---- */
static void test_persist(const char *stage) {
    struct statfs sf;
    char buf[128];
    struct stat st;
    CHECK("/ is the installed btrfs volume", statfs("/", &sf) == 0 && sf.f_type == 0x9123683E);
    CHECK("/tmp is still memory", statfs("/tmp", &sf) == 0 && sf.f_type != 0x9123683E);
    if (!strcmp(stage, "write")) {
        mkdir("/usr", 0755);   /* only DEVTOOLS images have it */
        CHECK("files anywhere: /usr/local, /opt, /var", mkdir("/usr/local", 0755) == 0 && mkdir("/usr/local/persist", 0755) == 0 &&
              write_file("/usr/local/persist/data", "kept across restarts\n", 0640) && mkdir("/opt", 0755) == 0 &&
              symlink("/usr/local/persist/data", "/opt/data-link") == 0 && link("/usr/local/persist/data", "/usr/local/persist/hard") == 0 &&
              mkdir("/var/private", 0700) == 0);
        CHECK("a program installed under /usr/local/bin", mkdir("/usr/local/bin", 0755) == 0 &&
              write_file("/usr/local/bin/hi", "#!/bin/echo persisted\n", 0755));
        CHECK("change a file of the system image (/etc/motd)", write_file("/etc/motd", "my own motd\n", 0644));
        CHECK("home and /tmp", write_file("/home/flux/persist-home", "home\n", 0600) && write_file("/tmp/should-vanish", "x", 0644));
        sync();
        return;
    }
    CHECK("a file in /usr/local survived, with its mode", read_file("/usr/local/persist/data", buf, sizeof buf) && !strcmp(buf, "kept across restarts\n") &&
          stat("/usr/local/persist/data", &st) == 0 && (st.st_mode & 07777) == 0640 && st.st_nlink == 2);
    CHECK("a symbolic link in /opt survived", readlink("/opt/data-link", buf, sizeof buf) == 23 && read_file("/opt/data-link", buf, sizeof buf) && !strcmp(buf, "kept across restarts\n"));
    CHECK("a directory's mode survived", stat("/var/private", &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & 07777) == 0700);
    CHECK("an installed program still runs", run_capture((char *[]){ "/usr/local/bin/hi", NULL }, buf, sizeof buf) == 0 && !strcmp(buf, "persisted /usr/local/bin/hi\n"));
    CHECK("the home directory survived", read_file("/home/flux/persist-home", buf, sizeof buf) && !strcmp(buf, "home\n"));
    CHECK("/tmp starts empty", access("/tmp/should-vanish", F_OK) == -1 && errno == ENOENT);
    CHECK("system update: a new file arrived", read_file("/etc/persist-image-added", buf, sizeof buf) && !strcmp(buf, "added by the update\n"));
    CHECK("system update: a file nobody changed was updated", read_file("/etc/hosts", buf, sizeof buf) && !strcmp(buf, "127.0.0.1 localhost updated\n"));
    CHECK("system update: a file the user changed was kept", read_file("/etc/motd", buf, sizeof buf) && !strcmp(buf, "my own motd\n"));
    int fd = open("/dev/null", O_WRONLY);
    CHECK("/dev still works", fd >= 0 && write(fd, "x", 1) == 1);
    if (fd >= 0) close(fd);
    CHECK("other disks still mount under /mnt (the EFI partition)", access("/mnt/.sda2/limine.conf", F_OK) == 0);
}

/* ---- pipes, poll, socketpair ---- */
static void test_ipc(void) {
    int p[2]; pipe2(p, O_NONBLOCK);
    char b;
    CHECK("non-blocking empty pipe: EAGAIN", read(p[0], &b, 1) == -1 && errno == EAGAIN);
    struct pollfd pf = { p[0], POLLIN, 0 };
    CHECK("poll timeout on an empty pipe", poll(&pf, 1, 50) == 0);
    write(p[1], "x", 1);
    CHECK("poll sees data", poll(&pf, 1, 0) == 1 && (pf.revents & POLLIN));
    close(p[1]);
    read(p[0], &b, 1);
    pf.revents = 0;
    CHECK("poll sees the writer gone (POLLHUP)", poll(&pf, 1, 0) == 1 && (pf.revents & POLLHUP));
    close(p[0]);
    signal(SIGPIPE, SIG_IGN);
    pipe(p); close(p[0]);
    CHECK("write to a pipe with no reader: EPIPE", write(p[1], "x", 1) == -1 && errno == EPIPE);
    close(p[1]);
    int sv[2];
    CHECK("socketpair", socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    char m[8] = { 0 };
    CHECK("socketpair both directions", write(sv[0], "ping", 4) == 4 && read(sv[1], m, 4) == 4 && !strcmp(m, "ping") && write(sv[1], "pong", 4) == 4 && read(sv[0], m, 4) == 4 && !strcmp(m, "pong"));
    close(sv[0]); close(sv[1]);
}

/* ---- time and randomness ---- */
static void test_time(void) {
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    struct timespec ts = { 0, 20000000 };
    nanosleep(&ts, NULL);
    clock_gettime(CLOCK_MONOTONIC, &b);
    long ns = (b.tv_sec - a.tv_sec) * 1000000000L + (b.tv_nsec - a.tv_nsec);
    CHECK("nanosleep 20 ms, measured by CLOCK_MONOTONIC", ns >= 19000000 && ns < 200000000);
    struct timespec rt; clock_gettime(CLOCK_REALTIME, &rt);
    CHECK("CLOCK_REALTIME is after 2024", rt.tv_sec > 1704067200);
    unsigned char r1[32], r2[32];
    CHECK("getrandom", getrandom(r1, 32, 0) == 32 && getrandom(r2, 32, 0) == 32 && memcmp(r1, r2, 32));
}

/* ---- loopback: TCP and UDP to 127.0.0.1, no network needed ---- */
static void test_loopback(void) {
    int l = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(5555), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    bool ok = l >= 0 && bind(l, (struct sockaddr *)&sa, sizeof sa) == 0 && listen(l, 4) == 0;
    CHECK("TCP listen on 127.0.0.1", ok);
    int c = socket(AF_INET, SOCK_STREAM, 0);
    CHECK("TCP connect over loopback", connect(c, (struct sockaddr *)&sa, sizeof sa) == 0);
    struct sockaddr_in peer; socklen_t pl = sizeof peer;
    int a = accept(l, (struct sockaddr *)&peer, &pl);
    CHECK("accept", a >= 0 && peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    char big[100000];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (char)(i * 7);
    pid_t w = fork();
    if (w == 0) { close(a); size_t o = 0; while (o < sizeof big) { ssize_t r = write(c, big + o, sizeof big - o); if (r <= 0) _exit(1); o += (size_t)r; } close(c); _exit(0); }
    close(c);
    static char got[100000]; size_t n = 0; ssize_t r;
    while ((r = read(a, got + n, sizeof got - n)) > 0) n += (size_t)r;
    int st; waitpid(w, &st, 0);
    CHECK("100 kB across loopback TCP, then EOF", n == sizeof big && !memcmp(got, big, sizeof big) && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    close(a); close(l);
    int u1 = socket(AF_INET, SOCK_DGRAM, 0), u2 = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in ua = { .sin_family = AF_INET, .sin_port = htons(5556), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    bind(u1, (struct sockaddr *)&ua, sizeof ua);
    char m[16] = { 0 };
    CHECK("UDP over loopback", sendto(u2, "dgram", 5, 0, (struct sockaddr *)&ua, sizeof ua) == 5 && recv(u1, m, sizeof m, 0) == 5 && !strcmp(m, "dgram"));
    close(u1); close(u2);
}

/* ---- network (optional) ---- */
static void test_net(void) {
    struct addrinfo hints = { 0 }, *ai = NULL;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    int e = getaddrinfo("example.com", "80", &hints, &ai);
    CHECK("getaddrinfo example.com (DNS over UDP from user mode)", e == 0 && ai);
    if (e || !ai) return;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    CHECK("TCP connect", s >= 0 && connect(s, ai->ai_addr, ai->ai_addrlen) == 0);
    const char *req = "HEAD / HTTP/1.0\r\nHost: example.com\r\n\r\n";
    char buf[512] = { 0 };
    ssize_t n = 0, r;
    if (send(s, req, strlen(req), 0) == (ssize_t)strlen(req)) while ((r = recv(s, buf + n, sizeof buf - 1 - n, 0)) > 0) n += r;
    CHECK("HTTP over a user-mode socket", n > 12 && !strncmp(buf, "HTTP/1.", 7));
    close(s);
    /* non-blocking connect */
    s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int c = connect(s, ai->ai_addr, ai->ai_addrlen);
    struct pollfd pf = { s, POLLOUT, 0 };
    int err = -1; socklen_t el = sizeof err;
    CHECK("non-blocking connect: EINPROGRESS, then POLLOUT, SO_ERROR 0", c == -1 && errno == EINPROGRESS && poll(&pf, 1, 5000) == 1 && getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0);
    close(s);
    freeaddrinfo(ai);
    /* HTTPS through /bin/wget: BearSSL, certificate checks against the Mozilla roots, the system clock */
    int st;
    pid_t w = fork();
    if (w == 0) { int dn = open("/dev/null", O_WRONLY); dup2(dn, 2); execl("/bin/wget", "wget", "-q", "-O", "/tmp/https.html", "https://example.com/", (char *)NULL); _exit(127); }
    waitpid(w, &st, 0);
    char page[4096] = { 0 };
    int hf = open("/tmp/https.html", O_RDONLY);
    if (hf >= 0) { read(hf, page, sizeof page - 1); close(hf); }
    CHECK("HTTPS GET (wget, BearSSL, certificate verified)", WIFEXITED(st) && WEXITSTATUS(st) == 0 && strstr(page, "Example Domain"));
    w = fork();
    if (w == 0) { int dn = open("/dev/null", O_WRONLY); dup2(dn, 2); execl("/bin/wget", "wget", "-q", "-O", "/dev/null", "https://expired.badssl.com/", (char *)NULL); _exit(127); }
    waitpid(w, &st, 0);
    CHECK("HTTPS refuses an expired certificate", WIFEXITED(st) && WEXITSTATUS(st) == 1);
    /* a listening socket that QEMU's hostfwd can reach (tcp::5555-:7) answers one connection */
    int l = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(7), .sin_addr.s_addr = INADDR_ANY };
    CHECK("bind + listen", bind(l, (struct sockaddr *)&sa, sizeof sa) == 0 && listen(l, 4) == 0);
    close(l);
}

/* ---- a volume made by tests/mk-fs-image.sh (ext4, NTFS), mounted at dir ---- */
static bool pattern_ok(const char *path, long size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    static unsigned char buf[65536];
    long off = 0; ssize_t r; bool ok = true;
    while ((r = read(fd, buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i < r; i++) if (buf[i] != (unsigned char)(((off + i) * 7 + size) % 251)) ok = false;
        off += r;
    }
    close(fd);
    return ok && off == size;
}
static void test_volume(const char *dir) {
    char p[512];
    static const long sizes[] = { 0, 1, 4095, 4096, 70000, 5 * 1024 * 1024 + 3 };
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        snprintf(p, sizeof p, "%s/p%ld", dir, sizes[i]);
        char name[64]; snprintf(name, sizeof name, "read a %ld-byte file", sizes[i]);
        struct stat st;
        CHECK(name, stat(p, &st) == 0 && st.st_size == sizes[i] && pattern_ok(p, sizes[i]));
    }
    snprintf(p, sizeof p, "%s/dir/sub/deep.bin", dir);
    CHECK("nested directories", pattern_ok(p, 12345));
    snprintf(p, sizeof p, "%s/many", dir);
    DIR *d = opendir(p); int n = 0; struct dirent *e; bool all = true;
    while (d && (e = readdir(d))) if (strncmp(e->d_name, "file", 4) == 0) n++;
    if (d) closedir(d);
    for (int i = 0; i < 300; i += 37) { snprintf(p, sizeof p, "%s/many/file%03d", dir, i); char b[8] = { 0 }; int fd = open(p, O_RDONLY); if (fd < 0 || read(fd, b, 7) <= 0 || atoi(b) != i) all = false; if (fd >= 0) close(fd); }
    CHECK("a 300-entry directory: listing and lookups", n == 300 && all);
    snprintf(p, sizeof p, "%s/short-link", dir);
    struct stat ls;
    if (lstat(p, &ls) == 0) {
        char t[256] = { 0 };
        CHECK("short symbolic link", S_ISLNK(ls.st_mode) && readlink(p, t, sizeof t) == 5 && !strcmp(t, "p4096") && pattern_ok(p, 4096));
        snprintf(p, sizeof p, "%s/long-link", dir);
        memset(t, 0, sizeof t);
        CHECK("long symbolic link (stored in a block)", readlink(p, t, sizeof t) == 83 && !strncmp(t, "dir/sub/../sub/deep.bin-that", 28));
    }
    CHECK("the volume is read-only", open(dir, O_RDONLY) >= 0 && ({ snprintf(p, sizeof p, "%s/new.txt", dir); open(p, O_CREAT | O_WRONLY, 0644) < 0; }));
}

int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "posix")) {
        setvbuf(stdout, NULL, _IOLBF, 0);
        printf("systest: POSIX file checks in %s\n", argv[2]);
        test_posix(argv[2]);
        printf("systest: %d of %d checks passed\n", checks - failures, checks);
        return failures;
    }
    if (argc > 2 && !strcmp(argv[1], "persist")) {
        setvbuf(stdout, NULL, _IOLBF, 0);
        printf("systest: persistence checks (%s)\n", argv[2]);
        test_persist(argv[2]);
        printf("systest: %d of %d checks passed\n", checks - failures, checks);
        return failures;
    }
    if (argc > 1 && !strcmp(argv[1], "devtools")) {
        setvbuf(stdout, NULL, _IOLBF, 0);
        printf("systest: developer tools checks\n");
        test_devtools();
        printf("systest: %d of %d checks passed\n", checks - failures, checks);
        return failures;
    }
    if (argc > 2 && !strcmp(argv[1], "fs")) {
        setvbuf(stdout, NULL, _IOLBF, 0);
        printf("systest: checking the volume at %s\n", argv[2]);
        test_volume(argv[2]);
        printf("systest: %d of %d checks passed\n", checks - failures, checks);
        return failures;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("systest: inFlux system call checks (pid %d)\n", getpid());
    test_memory();
    test_signals();
    test_processes();
    test_threads();
    test_files();
    test_symlinks();
    test_exec();
    test_posix("/tmp");
    test_ipc();
    test_time();
    test_timers();
    test_loopback();
    if (argc > 1 && !strcmp(argv[1], "net")) test_net();
    printf("systest: %d of %d checks passed\n", checks - failures, checks);
    return failures;
}
