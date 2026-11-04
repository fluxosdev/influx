.section .text
.extern syscall_dispatch
.extern isr_exit
.global syscall_entry, user_enter
/* Entered with interrupts masked (SFMASK). rcx = user rip, r11 = user rflags, rsp = user stack.
 * Builds a struct regs (arch.h), the same frame the interrupt stubs push, so system calls,
 * signals, fork and thread start share one user context format. syscall_dispatch returns
 * nonzero when the frame was changed in ways sysret cannot restore (rcx/r11 from a signal
 * return, a new image): then the exit goes through iretq instead.
 * The per-CPU offsets are CPU_OFF_SYSCALL_RSP / CPU_OFF_USER_RSP in percpu.h. */
syscall_entry:
    swapgs
    movq %rsp, %gs:24              /* user rsp, until it is on the kernel stack */
    movq %gs:16, %rsp              /* this thread's kernel stack */
    pushq $0x1b                    /* ss */
    pushq %gs:24                   /* rsp */
    pushq %r11                     /* rflags */
    pushq $0x23                    /* cs */
    pushq %rcx                     /* rip */
    pushq $0                       /* err_code */
    pushq $-1                      /* int_no: -1 marks a system call frame */
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    sti
    cld
    movq %rsp, %rdi
    call syscall_dispatch
    cli
    testq %rax, %rax
    jnz 1f
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11                      /* replaced by rflags below */
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx                      /* replaced by rip below */
    popq %rbx
    popq %rax
    addq $16, %rsp                 /* int_no, err_code */
    popq %rcx                      /* rip */
    addq $8, %rsp                  /* cs */
    popq %r11                      /* rflags */
    swapgs
    popq %rsp
    sysretq
1:  jmp isr_exit                   /* swapgs + iretq, as cs is a user selector */

/* void user_enter(struct regs *r): drop everything on this kernel stack and iretq to r */
user_enter:
    cli
    movq %rdi, %rsp
    jmp isr_exit

.section .note.GNU-stack,"",@progbits
