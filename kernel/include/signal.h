#pragma once
/* Signals: Linux numbering and x86-64 frame layout, so libc code written for Linux works. */
#include "kernel.h"

#define NSIG 64
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGSTKFLT 16
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGURG 23
#define SIGXCPU 24
#define SIGXFSZ 25
#define SIGVTALRM 26
#define SIGPROF 27
#define SIGWINCH 28
#define SIGIO 29
#define SIGPWR 30
#define SIGSYS 31

#define SIG_DFL 0
#define SIG_IGN 1

#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_RESTORER  0x04000000
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define SIGBIT(s) (1ULL << ((s) - 1))
#define SIG_UNBLOCKABLE (SIGBIT(SIGKILL) | SIGBIT(SIGSTOP))

struct k_sigaction { uint64_t handler; uint64_t flags; uint64_t restorer; uint64_t mask; };

/* si_code values */
#define SI_USER 0
#define SI_KERNEL 0x80
#define SI_TKILL (-6)
#define SEGV_MAPERR 1
#define SEGV_ACCERR 2
#define CLD_EXITED 1
#define CLD_KILLED 2

struct process;
struct thread;
struct regs;
/* queue sig for the process (any thread not blocking it); false if p is gone */
bool signal_send(struct process *p, int sig);
void signal_send_thread(struct thread *t, int sig);
/* on the way back to user mode: delivers pending signals by building frames on the user stack */
void signal_deliver(struct regs *r);
/* a fault in user mode that demand paging could not resolve */
void signal_fault(struct regs *r, int sig, int code, uint64_t addr);
int64_t sys_rt_sigreturn(struct regs *r);
bool signal_interrupted(void);   /* a blocking wait should give up with -EINTR */
