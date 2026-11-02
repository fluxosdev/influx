.section .text.start
.global _start
_start:
    movq (%rsp), %rdi        /* argc */
    leaq 8(%rsp), %rsi       /* argv */
    andq $-16, %rsp
    call __libc_start
    movq %rax, %rdi
    call exit
1:  jmp 1b
.section .note.GNU-stack,"",@progbits
