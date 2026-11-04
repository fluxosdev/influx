/* Copies between kernel and user memory. A page fault inside [uaccess_start, uaccess_end)
 * that demand paging cannot satisfy resumes at uaccess_fault, which returns -EFAULT
 * (see the page fault handler in idt.c). The callers in uaccess.h check that the user
 * range lies below USER_MAX first, so these never touch kernel memory on a user's behalf. */
.section .text
.global uaccess_start, uaccess_end, uaccess_fault
.global __copy_user, __strncpy_from_user, __clear_user

uaccess_start:
/* long __copy_user(void *dst, const void *src, size_t n): 0 or -EFAULT */
__copy_user:
    movq %rdx, %rcx
    rep movsb
    xorl %eax, %eax
    ret

/* long __strncpy_from_user(char *dst, const char *src, size_t max): length, -ENAMETOOLONG or -EFAULT */
__strncpy_from_user:
    xorl %eax, %eax
1:  cmpq %rdx, %rax
    jae 2f
    movb (%rsi,%rax), %cl
    movb %cl, (%rdi,%rax)
    testb %cl, %cl
    jz 3f
    incq %rax
    jmp 1b
2:  movq $-36, %rax
3:  ret

/* long __clear_user(void *dst, size_t n) */
__clear_user:
    movq %rsi, %rcx
    xorl %eax, %eax
    rep stosb
    ret
uaccess_end:

uaccess_fault:
    movq $-14, %rax
    ret

.section .note.GNU-stack,"",@progbits
