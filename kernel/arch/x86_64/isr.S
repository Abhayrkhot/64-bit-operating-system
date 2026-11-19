// Interrupt entry. 256 stubs, 16 bytes apart starting at isr_stub_table, each
// pushing a dummy error code (when the CPU doesn't) and the vector number,
// then a common path that saves the GPRs into an InterruptFrame and calls
// interrupt_dispatch(frame). swapgs runs only when coming from ring 3.

.section .text
.align 16
.global isr_stub_table
isr_stub_table:
.set vec, 0
.rept 256
    .align 16
    .if (vec == 8) + (vec == 10) + (vec == 11) + (vec == 12) + (vec == 13) + (vec == 14) + (vec == 17) + (vec == 21) + (vec == 29) + (vec == 30)
    .else
        pushq $0
    .endif
    pushq $vec
    jmp isr_common
    .set vec, vec + 1
.endr

.align 16
isr_common:
    testb $3, 24(%rsp)          // CS of the interrupted context
    jz 1f
    swapgs
1:
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
    mov %rsp, %rdi
    cld
    call interrupt_dispatch
.global interrupt_return
interrupt_return:
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
    testb $3, 24(%rsp)
    jz 2f
    swapgs
2:
    addq $16, %rsp              // vector + error code
    iretq
