// Process entry: the kernel passes argc in RDI and argv in RSI.
.section .text.start, "ax"
.global _start
_start:
    xor %ebp, %ebp
    and $-16, %rsp
    call __libc_start
    ud2
