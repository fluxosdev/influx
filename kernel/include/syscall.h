#pragma once
/* Helpers shared by the system call modules (syscall.c, net/socket.c). */
#include "sched.h"
#include "fs.h"
#include "uaccess.h"

struct file *fd_get(struct process *p, int fd);
/* installs f in the lowest free slot (at least min); returns the descriptor or -EMFILE (f is closed then) */
int fd_install(struct process *p, struct file *f, bool cloexec, int min);
/* user-memory bounce buffers for transfers */
#define XFER_CHUNK (64 * 1024)

/* sockets (net/socket.c) */
int64_t sys_socket(int domain, int type, int proto);
int64_t sys_socketpair(int domain, int type, int proto, uint64_t usv);
int64_t sys_bind(int fd, uint64_t uaddr, uint32_t len);
int64_t sys_listen(int fd, int backlog);
int64_t sys_accept4(int fd, uint64_t uaddr, uint64_t ulen, int flags);
int64_t sys_connect(int fd, uint64_t uaddr, uint32_t len);
int64_t sys_sendto(int fd, uint64_t buf, size_t len, int flags, uint64_t uaddr, uint32_t alen);
int64_t sys_recvfrom(int fd, uint64_t buf, size_t len, int flags, uint64_t uaddr, uint64_t ualen);
int64_t sys_sendmsg(int fd, uint64_t umsg, int flags);
int64_t sys_recvmsg(int fd, uint64_t umsg, int flags);
int64_t sys_shutdown(int fd, int how);
int64_t sys_getsockname(int fd, uint64_t uaddr, uint64_t ulen, bool peer);
int64_t sys_setsockopt(int fd, int level, int opt, uint64_t uval, uint32_t len);
int64_t sys_getsockopt(int fd, int level, int opt, uint64_t uval, uint64_t ulen);
bool file_is_socket(struct file *f);

/* futex.c */
int64_t sys_futex(uint64_t uaddr, int op, uint32_t val, uint64_t utimeout, uint64_t uaddr2, uint32_t val3);
int futex_wake(uint64_t uaddr, int n);

/* exec.c */
int process_execve(const char *path, int argc, char *const *argv, int envc, char *const *envp, struct regs *r);
int process_spawn_env(const char *path, int argc, char **argv, char *const *envp, struct file *stdio[3], const char *cwd);
