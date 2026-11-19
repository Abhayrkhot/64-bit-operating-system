// void context_switch(uint64_t* save_rsp, uint64_t load_rsp)
// Saves the callee-saved registers on the current stack, stores RSP into
// *save_rsp, loads load_rsp and pops the next thread's registers. Returns
// into whatever the next thread's stack holds (its own context_switch call,
// or thread_trampoline for a new thread).

.section .text
.global context_switch
.type context_switch, @function
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
.size context_switch, . - context_switch

// First code a new thread runs: r12 = entry, r13 = arg (set up by sched.cpp).
.global thread_trampoline
.type thread_trampoline, @function
thread_trampoline:
    call sched_thread_start      // finish the switch, enable interrupts
    movq %r13, %rdi
    xor %ebp, %ebp
    call *%r12
    movl $0, %edi
    call sched_thread_exit
    ud2
.size thread_trampoline, . - thread_trampoline
