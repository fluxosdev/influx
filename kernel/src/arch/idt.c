#include "arch.h"
#include "string.h"
#include "io.h"
#include "sched.h"
#include "uvm.h"
#include "uaccess.h"
#include "fs.h"
#include "random.h"
#include "signal.h"

extern char uaccess_start[], uaccess_end[], uaccess_fault[];

struct idt_entry { uint16_t off0; uint16_t sel; uint8_t ist; uint8_t type; uint16_t off1; uint32_t off2; uint32_t zero; } PACKED;
static struct idt_entry idt[256] ALIGNED(16);
static struct { uint16_t limit; uint64_t base; } PACKED idtr;
extern void *isr_table[256];

static struct { irq_handler_t h; void *ctx; } handlers[256];

static const char *exc_names[32] = {
    "Divide by zero", "Debug", "NMI", "Breakpoint", "Overflow", "Bound range", "Invalid opcode",
    "Device not available", "Double fault", "Coprocessor overrun", "Invalid TSS", "Segment not present",
    "Stack fault", "General protection fault", "Page fault", "Reserved", "x87 FP exception",
    "Alignment check", "Machine check", "SIMD FP exception", "Virtualization", "Control protection",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Hypervisor injection",
    "VMM communication", "Security exception", "Reserved" };

void idt_set_gate(uint8_t vec, void *handler, uint8_t ist, uint8_t dpl) {
    uint64_t a = (uint64_t)handler;
    idt[vec].off0 = a & 0xFFFF;
    idt[vec].sel = KERNEL_CS;
    idt[vec].ist = ist;
    idt[vec].type = 0x8E | ((dpl & 3) << 5);   /* present, interrupt gate */
    idt[vec].off1 = (a >> 16) & 0xFFFF;
    idt[vec].off2 = a >> 32;
    idt[vec].zero = 0;
}

void idt_init(void) {
    memset(idt, 0, sizeof idt);
    for (int i = 0; i < 256; i++) idt_set_gate(i, isr_table[i], 0, 0);
    idt_set_gate(3, isr_table[3], 0, 3);   /* int3 usable from user mode */
    idt_set_gate(8, isr_table[8], 1, 0);   /* IST stacks set up in gdt_init */
    idt_set_gate(2, isr_table[2], 2, 0);
    idt_set_gate(18, isr_table[18], 3, 0);
    idtr.limit = sizeof idt - 1;
    idtr.base = (uint64_t)idt;
    idt_load();
}

void idt_load(void) { __asm__ volatile("lidt %0" :: "m"(idtr)); }

void irq_register(uint8_t vec, irq_handler_t h, void *ctx) { handlers[vec].h = h; handlers[vec].ctx = ctx; }

/* 0x20-0x2F legacy IRQs, 0x40 the LAPIC timer; devices get 0x50 and up */
int irq_alloc_vector(void) {
    static int next = 0x50;
    int v = __atomic_fetch_add(&next, 1, __ATOMIC_RELAXED);
    return v < 0xF0 ? v : 0;
}

static void dump_regs(struct regs *r) {
    kprintf("RIP=%016lx CS=%04lx RFLAGS=%08lx RSP=%016lx SS=%04lx\n", r->rip, r->cs, r->rflags, r->rsp, r->ss);
    kprintf("RAX=%016lx RBX=%016lx RCX=%016lx RDX=%016lx\n", r->rax, r->rbx, r->rcx, r->rdx);
    kprintf("RSI=%016lx RDI=%016lx RBP=%016lx R8 =%016lx\n", r->rsi, r->rdi, r->rbp, r->r8);
    kprintf("R9 =%016lx R10=%016lx R11=%016lx R12=%016lx\n", r->r9, r->r10, r->r11, r->r12);
    kprintf("R13=%016lx R14=%016lx R15=%016lx ERR=%016lx\n", r->r13, r->r14, r->r15, r->err_code);
    if (r->int_no == 14) kprintf("CR2=%016lx\n", read_cr2());
}

/* #PF: demand paging and copy-on-write for user memory, from user mode or from the kernel's
 * user-access routines; true when the faulting access can simply be retried */
static bool page_fault_user_memory(struct regs *r, uint64_t cr2) {
    struct thread *t = current_thread;
    if (!t || !t->proc || !t->proc->is_user || cr2 >= USER_MAX) return false;
    bool write = r->err_code & 2, exec = r->err_code & 16;
    return uvm_fault(t->proc, cr2, write, exec) == 0;
}

void isr_dispatch(struct regs *r) {
    if (r->int_no < 32) {
        /* NMI, #DF and #MC run on IST stacks, which must never be left via schedule() */
        if (r->int_no == 2 && (r->cs & 3)) { kprintf("NMI in user mode at rip=%lx\n", r->rip); return; }
        if (r->int_no == 14) {
            uint64_t cr2 = read_cr2();
            if (page_fault_user_memory(r, cr2)) { if (r->cs & 3) signal_deliver(r); return; }
            /* the kernel's copy_from_user and friends: give up with -EFAULT */
            if (!(r->cs & 3) && r->rip >= (uint64_t)uaccess_start && r->rip < (uint64_t)uaccess_end) { r->rip = (uint64_t)uaccess_fault; return; }
        }
        if ((r->cs & 3) && r->int_no != 8 && r->int_no != 18) {
            /* fault in user mode: a signal, which kills the process unless it handles it */
            int sig, code = 0; uint64_t addr = r->rip;
            switch (r->int_no) {
            case 0: case 16: case 19: sig = SIGFPE; break;
            case 1: case 3: sig = SIGTRAP; break;
            case 6: sig = SIGILL; break;
            case 14: sig = SIGSEGV; addr = read_cr2(); code = (r->err_code & 1) ? 2 /* SEGV_ACCERR */ : 1 /* SEGV_MAPERR */;
                     if (uvm_fault(current_thread->proc, addr, false, false) == -ENOMEM) sig = SIGBUS; break;
            case 17: sig = SIGBUS; break;
            default: sig = SIGSEGV; addr = 0; code = 0x80; break;
            }
            signal_fault(r, sig, code, addr);
            return;
        }
        kprintf("\n\033[31;1m*** EXCEPTION %lu: %s ***\033[0m\n", r->int_no, exc_names[r->int_no]);
        dump_regs(r);
        if (current_thread) kprintf("thread '%s' pid %d\n", current_thread->name, current_thread->proc ? current_thread->proc->pid : -1);
        kprintf("backtrace:\n");
        panic_backtrace(r->rip, r->rbp);
        if (r->int_no == 14) panic("%s at %016lx (address %016lx)", exc_names[r->int_no], r->rip, read_cr2());
        panic("%s at %016lx", exc_names[r->int_no], r->rip);
    }
    /* the interrupted thread's vector registers are live and unsaved (simd_ok) */
    struct thread *t = current_thread;
    if (t) t->irq_depth++;
    if (r->int_no != VEC_LAPIC_TIMER) random_add_entropy(r->int_no);   /* device interrupt timing */
    if (handlers[r->int_no].h) handlers[r->int_no].h(r, handlers[r->int_no].ctx);
    else if (r->int_no != VEC_SPURIOUS) { lapic_eoi(); }
    if (t) t->irq_depth--;
    /* back to user mode: pending signals first */
    if ((r->cs & 3) && t == current_thread && t && t->is_user && (t->kill_pending || process_has_signal(t->proc, t))) signal_deliver(r);
}
