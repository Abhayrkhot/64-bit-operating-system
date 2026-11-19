// Permission probes with exception-table fixups. See extable.hpp.

.section .text

.global probe_read
probe_read:
    xor %eax, %eax
1:  movb (%rdi), %cl
    ret
2:  mov $1, %eax
    ret
.pushsection .extable, "a"
    .quad 1b, 2b
.popsection

.global probe_write
probe_write:
    xor %eax, %eax
3:  movb (%rdi), %cl
4:  movb %cl, (%rdi)
    ret
5:  mov $1, %eax
    ret
.pushsection .extable, "a"
    .quad 3b, 5b
    .quad 4b, 5b
.popsection

// probe_exec: calls the target. A fetch fault at the target is recognised by
// extable::fixup (RIP == probe_exec_target) and unwound to probe_exec_fault.
.global probe_exec
probe_exec:
    push %rbp
    mov %rsp, %rbp
    mov %rdi, probe_exec_target(%rip)
    call *%rdi
    movq $0, probe_exec_target(%rip)
    pop %rbp
    ret
.global probe_exec_fault
probe_exec_fault:
    movq $0, probe_exec_target(%rip)
    mov $1, %eax
    mov %rbp, %rsp
    pop %rbp
    ret

.section .bss
.align 8
.global probe_exec_target
probe_exec_target:
    .quad 0

// A harmless function for probe_exec's positive case.
.section .text
.global probe_exec_ok
probe_exec_ok:
    xor %eax, %eax
    ret
