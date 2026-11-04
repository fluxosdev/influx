/*
 * POSIX signals.
 *
 * A signal sent to a process is pending on it until a thread that does not
 * block it returns to user mode; faults and tgkill target one thread. On the
 * way out of a system call or interrupt, signal_deliver() takes one pending
 * signal: the default action (ignore or terminate), or a frame on the user
 * stack laid out like Linux's x86-64 rt_sigframe (return address = the
 * sa_restorer, then the ucontext with the interrupted registers, siginfo and
 * the XSAVE image), with the handler entered as handler(sig, &info, &uc).
 * rt_sigreturn restores that context, checked, through iretq.
 *
 * A blocking system call interrupted by a signal returns -ERESTARTSYS; if a
 * handler runs and lacks SA_RESTART that becomes -EINTR, otherwise the call
 * is restarted by backing up over the syscall instruction.
 */
#include "sched.h"
#include "signal.h"
#include "uaccess.h"
#include "simd.h"
#include "string.h"
#include "mm.h"

#define ERESTARTSYS 512
#define EINTR 4

struct uc_stack { uint64_t ss_sp; int32_t ss_flags; int32_t pad; uint64_t ss_size; };
struct sigcontext {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15, rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2, fpstate, reserved1[8];
};
struct ucontext { uint64_t uc_flags, uc_link; struct uc_stack uc_stack; struct sigcontext uc_mcontext; uint64_t uc_sigmask; };
struct siginfo {
    int32_t si_signo, si_errno, si_code, pad;
    union {
        struct { int32_t pid; uint32_t uid; } kill;
        struct { uint64_t addr; } fault;
        struct { int32_t pid; uint32_t uid; int32_t status; int32_t pad; int64_t utime, stime; } chld;
        uint8_t raw[112];
    };
};
struct rt_sigframe { uint64_t pretcode; struct ucontext uc; struct siginfo info; };
_Static_assert(sizeof(struct sigcontext) == 256, "sigcontext layout");
_Static_assert(sizeof(struct siginfo) == 128, "siginfo layout");

#define SS_ONSTACK 1
#define SS_DISABLE 2
#define UC_FP_XSTATE 1

/* default action ignore */
static bool default_ignored(int sig) { return sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH || sig == SIGCONT || sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU; }

static bool discarded(struct process *p, int sig) {
    if (sig == SIGKILL) return false;
    uint64_t h = p->sigact[sig].handler;
    return h == SIG_IGN || (h == SIG_DFL && default_ignored(sig));
}

/* wake t if it sleeps in an interruptible wait */
static void kick(struct thread *t) { thread_unblock(t); }

bool signal_send(struct process *p, int sig) {
    if (sig < 1 || sig > NSIG) return false;
    if (sig == SIGKILL) return process_kill(p->pid, SIGKILL);
    procs_lock();
    if (p->exited || discarded(p, sig)) { bool ok = !p->exited; procs_unlock(); return ok; }
    __atomic_fetch_or(&p->sig_pending, SIGBIT(sig), __ATOMIC_SEQ_CST);
    /* wake a thread that will take it; if all block it, it waits */
    struct thread *t;
    list_for_each_entry(t, &p->threads, proc_link) if (!(t->sigmask & SIGBIT(sig))) { kick(t); break; }
    procs_unlock();
    return true;
}

void signal_send_thread(struct thread *t, int sig) {
    if (sig < 1 || sig > NSIG) return;
    if (sig == SIGKILL) { process_kill(t->proc->pid, SIGKILL); return; }
    if (discarded(t->proc, sig)) return;
    __atomic_fetch_or(&t->sig_pending, SIGBIT(sig), __ATOMIC_SEQ_CST);
    kick(t);
}

bool signal_interrupted(void) {
    struct thread *t = current_thread;
    if (t->kill_pending) return true;
    if (!t->is_user) return false;
    return process_has_signal(t->proc, t);
}

/* takes the lowest deliverable signal (thread-directed first) */
static int dequeue(struct thread *t) {
    struct process *p = t->proc;
    uint64_t m = t->sig_pending & ~t->sigmask;
    if (m) { int s = __builtin_ctzll(m) + 1; __atomic_fetch_and(&t->sig_pending, ~SIGBIT(s), __ATOMIC_SEQ_CST); return s; }
    for (;;) {
        uint64_t pend = __atomic_load_n(&p->sig_pending, __ATOMIC_SEQ_CST);
        m = pend & ~t->sigmask;
        if (!m) return 0;
        int s = __builtin_ctzll(m) + 1;
        if (__atomic_compare_exchange_n(&p->sig_pending, &pend, pend & ~SIGBIT(s), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return s;
    }
}

static void NORETURN terminate(int sig) {
    struct process *p = current_thread->proc;
    p->exit_signal = sig;
    sti();
    process_exit(128 + sig);
}

/* the thread's last fault, for the siginfo of a synchronous signal */
static struct { int sig, code; uint64_t addr; } fault_info[MAX_CPUS];

static void fill_mcontext(struct sigcontext *sc, const struct regs *r) {
    sc->r8 = r->r8; sc->r9 = r->r9; sc->r10 = r->r10; sc->r11 = r->r11; sc->r12 = r->r12; sc->r13 = r->r13; sc->r14 = r->r14; sc->r15 = r->r15;
    sc->rdi = r->rdi; sc->rsi = r->rsi; sc->rbp = r->rbp; sc->rbx = r->rbx; sc->rdx = r->rdx; sc->rax = r->rax; sc->rcx = r->rcx;
    sc->rsp = r->rsp; sc->rip = r->rip; sc->eflags = r->rflags; sc->cs = (uint16_t)r->cs; sc->ss = (uint16_t)r->ss;
    sc->err = r->int_no == (uint64_t)-1 ? 0 : r->err_code; sc->trapno = r->int_no == (uint64_t)-1 ? 0 : r->int_no;
}

/* builds the frame; false if the stack is unusable */
static bool setup_frame(struct regs *r, int sig, struct k_sigaction *act, int code, uint64_t addr) {
    struct thread *t = current_thread;
    uint64_t sp = r->rsp;
    bool on_alt = t->alt_size && sp - t->alt_sp < t->alt_size;
    if ((act->flags & SA_ONSTACK) && t->alt_size && !on_alt) sp = t->alt_sp + t->alt_size;
    sp -= 128;                                  /* red zone */
    sp = (sp - fpu_state_size) & ~63ULL;
    uint64_t fp = sp;
    sp -= sizeof(struct rt_sigframe);
    sp = (sp & ~15ULL) - 8;                     /* the handler starts as if called: (rsp + 8) % 16 == 0 */
    struct rt_sigframe frame, *f = &frame;   /* on the kernel stack: this may run in an interrupt, where kmalloc must not sleep */
    memset(f, 0, sizeof *f);
    f->pretcode = act->restorer;
    f->uc.uc_flags = UC_FP_XSTATE;
    f->uc.uc_stack.ss_sp = t->alt_sp; f->uc.uc_stack.ss_size = t->alt_size;
    f->uc.uc_stack.ss_flags = !t->alt_size ? SS_DISABLE : on_alt ? SS_ONSTACK : 0;
    fill_mcontext(&f->uc.uc_mcontext, r);
    f->uc.uc_mcontext.fpstate = fp;
    f->uc.uc_mcontext.cr2 = addr;
    uint64_t mask = t->restore_sigmask ? t->saved_sigmask : t->sigmask;
    if (t->restore_sigmask) { t->sigmask = t->saved_sigmask; t->restore_sigmask = false; }   /* the handler's mask builds on the real one */
    f->uc.uc_mcontext.oldmask = mask;
    f->uc.uc_sigmask = mask;
    f->info.si_signo = sig; f->info.si_code = code;
    if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE || sig == SIGTRAP) f->info.fault.addr = addr;
    /* the interrupted thread's vector registers: live in the CPU until saved */
    uint64_t fl = irq_save();
    fpu_save(t->fpu);
    irq_restore(fl);
    if (copy_to_user(fp, t->fpu, fpu_state_size) < 0 || copy_to_user(sp, f, sizeof *f) < 0) return false;
    r->rip = act->handler; r->rsp = sp;
    r->rdi = (uint64_t)sig; r->rsi = sp + offsetof(struct rt_sigframe, info); r->rdx = sp + offsetof(struct rt_sigframe, uc);
    r->rax = 0;
    r->rflags &= ~(0x400ULL | 0x100ULL);   /* DF, TF */
    return true;
}

void signal_deliver(struct regs *r) {
    struct thread *t = current_thread;
    if (!t || !t->is_user || !(r->cs & 3)) return;
    struct process *p = t->proc;
    bool in_syscall = r->int_no == (uint64_t)-1;
    for (;;) {
        if (t->kill_pending) terminate(p->exit_signal ? p->exit_signal : SIGKILL);
        int sig = dequeue(t);
        if (!sig) break;
        struct k_sigaction act = p->sigact[sig];
        if (act.handler == SIG_IGN) continue;
        if (act.handler == SIG_DFL) { if (default_ignored(sig)) continue; terminate(sig); }
        int code = SI_USER; uint64_t addr = 0;
        struct cpu *c = this_cpu();
        if (fault_info[c->id].sig == sig) { code = fault_info[c->id].code; addr = fault_info[c->id].addr; fault_info[c->id].sig = 0; }
        if (!(act.flags & SA_RESTORER) || !act.restorer) terminate(sig);   /* no way back: as if unhandled */
        /* an interrupted system call: restart it after the handler, or report EINTR */
        if (in_syscall && (int64_t)r->rax == -ERESTARTSYS) {
            if (act.flags & SA_RESTART) { r->rax = (uint64_t)t->sys_nr; r->rip -= 2; }   /* back over the 2-byte syscall */
            else r->rax = (uint64_t)-EINTR;
        }
        if (!setup_frame(r, sig, &act, code, addr)) terminate(SIGSEGV);
        t->sigmask |= act.mask | (act.flags & SA_NODEFER ? 0 : SIGBIT(sig));
        t->sigmask &= ~SIG_UNBLOCKABLE;
        if (act.flags & SA_RESETHAND) p->sigact[sig].handler = SIG_DFL;
        return;   /* one frame at a time; the next is taken when this handler returns */
    }
}

void signal_fault(struct regs *r, int sig, int code, uint64_t addr) {
    struct thread *t = current_thread;
    struct process *p = t->proc;
    uint64_t h = p->sigact[sig].handler;
    if (h == SIG_DFL || h == SIG_IGN || (t->sigmask & SIGBIT(sig))) {
        kprintf("[proc %d '%s'] %s at rip=%lx addr=%lx -> killed\n", p->pid, p->name,
                sig == SIGSEGV ? "segmentation fault" : sig == SIGBUS ? "bus error" : sig == SIGILL ? "illegal instruction" : sig == SIGFPE ? "arithmetic exception" : "trap",
                r->rip, addr);
        terminate(sig);
    }
    struct cpu *c = this_cpu();
    fault_info[c->id].sig = sig; fault_info[c->id].code = code; fault_info[c->id].addr = addr;
    __atomic_fetch_or(&t->sig_pending, SIGBIT(sig), __ATOMIC_SEQ_CST);
    sti();
    signal_deliver(r);
}

/* the handler returned through its restorer: rsp points just past pretcode */
int64_t sys_rt_sigreturn(struct regs *r) {
    struct thread *t = current_thread;
    uint64_t sp = r->rsp - 8;
    struct ucontext uc;
    if (copy_from_user(&uc, sp + offsetof(struct rt_sigframe, uc), sizeof uc) < 0) terminate(SIGSEGV);
    struct sigcontext *sc = &uc.uc_mcontext;
    if (sc->rip >= USER_MAX) terminate(SIGSEGV);
    r->r8 = sc->r8; r->r9 = sc->r9; r->r10 = sc->r10; r->r11 = sc->r11; r->r12 = sc->r12; r->r13 = sc->r13; r->r14 = sc->r14; r->r15 = sc->r15;
    r->rdi = sc->rdi; r->rsi = sc->rsi; r->rbp = sc->rbp; r->rbx = sc->rbx; r->rdx = sc->rdx; r->rax = sc->rax; r->rcx = sc->rcx;
    r->rsp = sc->rsp; r->rip = sc->rip;
    /* user-changeable flags only (CF PF AF ZF SF TF DF OF AC ID); IF stays on */
    const uint64_t user_flags = 0x1 | 0x4 | 0x10 | 0x40 | 0x80 | 0x100 | 0x400 | 0x800 | 0x40000 | 0x200000;
    r->rflags = (r->rflags & ~user_flags) | (sc->eflags & user_flags) | 0x200;
    r->cs = USER_CS; r->ss = USER_DS;
    t->sigmask = uc.uc_sigmask & ~SIG_UNBLOCKABLE;
    /* vector state: accept only an image XRSTOR takes without faulting */
    if (sc->fpstate) {
        uint8_t *img = kmalloc(fpu_state_size + 64);
        uint8_t *a = (uint8_t *)ALIGN_UP((uint64_t)img, 64);
        if (img && copy_from_user(a, sc->fpstate, fpu_state_size) == 0) {
            uint32_t mxcsr = *(uint32_t *)(a + 24);
            uint64_t bv = *(uint64_t *)(a + 512), comp = *(uint64_t *)(a + 520);
            bool ok = !(mxcsr & 0xFFFF0000u) && !(bv & ~cpu_xcr0) && comp == 0;
            for (int i = 528; ok && i < 576; i++) if (a[i]) ok = false;   /* reserved header bytes */
            if (ok) {
                memcpy(t->fpu, a, fpu_state_size);
                uint64_t fl = irq_save();
                fpu_restore(t->fpu);
                irq_restore(fl);
            }
        }
        kfree(img);
    }
    t->iret_return = true;   /* rcx and r11 belong to the interrupted code */
    return (int64_t)r->rax;
}
