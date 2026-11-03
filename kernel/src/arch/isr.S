/* Interrupt stubs: 256 vectors funnelled into isr_dispatch(struct regs *) */
.section .text
.extern isr_dispatch

.macro ISR_NOERR n
.global isr\n
isr\n:
    pushq $0
    pushq $\n
    jmp isr_common
.endm
.macro ISR_ERR n
.global isr\n
isr\n:
    pushq $\n
    jmp isr_common
.endm

isr_common:
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
    cld
    movq %rsp, %rdi          /* the CPU 16-aligns rsp before its 5-qword frame; 2 + 15 pushes keep it aligned */
    call isr_dispatch
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp
    iretq

.altmacro
.macro GEN n
.if \n == 8 || \n == 10 || \n == 11 || \n == 12 || \n == 13 || \n == 14 || \n == 17 || \n == 21 || \n == 29 || \n == 30
    ISR_ERR \n
.else
    ISR_NOERR \n
.endif
.endm

.set i, 0
.rept 256
    GEN %i
    .set i, i + 1
.endr

.macro TBL n
    .quad isr\n
.endm

.section .rodata
.global isr_table
isr_table:
.set i, 0
.rept 256
    TBL %i
    .set i, i + 1
.endr

.section .note.GNU-stack,"",@progbits
