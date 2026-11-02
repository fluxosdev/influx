.section .text
.extern syscall_dispatch
.extern kernel_rsp_for_syscall
.global syscall_entry
/* Entered with interrupts masked (SFMASK). rcx = user rip, r11 = user rflags, rsp = user stack. */
syscall_entry:
    movq %rsp, user_rsp_scratch(%rip)
    movq kernel_rsp_for_syscall(%rip), %rsp
    pushq user_rsp_scratch(%rip)   /* user rsp */
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
    popq %rsp
    sysretq

.section .bss
user_rsp_scratch: .quad 0

.section .note.GNU-stack,"",@progbits
