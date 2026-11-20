// User-memory copies. Every access that can fault has an exception-table
// entry whose fixup returns -EFAULT, so a bad user pointer (unmapped,
// read-only for a write, or a kernel page) fails the syscall instead of
// crashing the kernel. Two variants: with SMAP the copy opens the user-access
// window with stac/clac; without SMAP those instructions would #UD.

.macro USERCOPY suffix, smap
.section .text
// int64_t user_copy_<suffix>(void* dst, const void* src, size_t n) -> 0 or -EFAULT
.global user_copy_\suffix
user_copy_\suffix:
    .if \smap
    stac
    .endif
    xor %eax, %eax
    mov %rdx, %rcx
1:  rep movsb
    .if \smap
    clac
    .endif
    ret
2:  .if \smap
    clac
    .endif
    mov $-14, %rax
    ret
.pushsection .extable, "a"
    .quad 1b, 2b
.popsection

// int64_t user_strncpy_<suffix>(char* dst, const char* src, size_t max)
//   -> length without the NUL, -EFAULT, or -ENAMETOOLONG if no NUL in max bytes
.global user_strncpy_\suffix
user_strncpy_\suffix:
    .if \smap
    stac
    .endif
    xor %eax, %eax
3:  cmp %rdx, %rax
    jae 6f
4:  movb (%rsi,%rax), %cl
    movb %cl, (%rdi,%rax)
    test %cl, %cl
    jz 5f
    inc %rax
    jmp 3b
5:  .if \smap
    clac
    .endif
    ret
6:  .if \smap
    clac
    .endif
    mov $-36, %rax
    ret
7:  .if \smap
    clac
    .endif
    mov $-14, %rax
    ret
.pushsection .extable, "a"
    .quad 4b, 7b
.popsection
.endm

USERCOPY smap, 1
USERCOPY nosmap, 0
