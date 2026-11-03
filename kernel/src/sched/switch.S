.section .text
/* void context_switch(uint64_t *old_rsp, uint64_t new_rsp) */
.global context_switch
context_switch:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret

/* void enter_user_mode(uint64_t rip, uint64_t rsp) */
.global enter_user_mode
enter_user_mode:
    cli
    movw $0x1b, %ax
    movw %ax, %ds
    movw %ax, %es
    pushq $0x1b          /* ss */
    pushq %rsi           /* rsp */
    pushq $0x202         /* rflags: IF */
    pushq $0x23          /* cs */
    pushq %rdi           /* rip */
    xorq %rax, %rax
    xorq %rbx, %rbx
    xorq %rcx, %rcx
    xorq %rdx, %rdx
    xorq %rsi, %rsi
    xorq %rdi, %rdi
    xorq %rbp, %rbp
    xorq %r8, %r8
    xorq %r9, %r9
    xorq %r10, %r10
    xorq %r11, %r11
    xorq %r12, %r12
    xorq %r13, %r13
    xorq %r14, %r14
    xorq %r15, %r15
    swapgs               /* user GS; the kernel's per-CPU pointer waits in KERNEL_GS_BASE */
    iretq

.section .note.GNU-stack,"",@progbits
