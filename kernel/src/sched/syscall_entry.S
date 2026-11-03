.section .text
.extern syscall_dispatch
.global syscall_entry
/* Entered with interrupts masked (SFMASK). rcx = user rip, r11 = user rflags, rsp = user stack.
 * The per-CPU offsets are CPU_OFF_SYSCALL_RSP / CPU_OFF_USER_RSP in percpu.h. */
syscall_entry:
    swapgs
    movq %rsp, %gs:24              /* user rsp, until it is on the kernel stack */
    movq %gs:16, %rsp              /* this thread's kernel stack */
    pushq %gs:24                   /* user rsp */
    pushq %rcx                     /* user rip */
    pushq %r11                     /* user rflags */
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %r10
    pushq %r8
    pushq %r9
    pushq %rax
    sti
    movq %rsp, %rdi
    call syscall_dispatch
    cli
    popq %rax
    popq %r9
    popq %r8
    popq %r10
    popq %rdx
    popq %rsi
    popq %rdi
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    popq %r11
    popq %rcx
    swapgs
    popq %rsp
    sysretq

.section .note.GNU-stack,"",@progbits
