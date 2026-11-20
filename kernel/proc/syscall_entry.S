// syscall/sysret entry and the first transition to ring 3.
//
// On `syscall` the CPU loads RIP from LSTAR, saves the user RIP in RCX and
// RFLAGS in R11, masks RFLAGS with FMASK (IF, TF, DF, AC, NT cleared) and
// switches CS/SS, but not RSP: the entry swaps GS to reach the per-CPU block,
// stashes the user RSP there and loads the thread's kernel stack.
//
// The saved state is laid out exactly like interrupts::Frame, so the slow
// return path can reuse interrupt_return (iretq).

.set PERCPU_KERNEL_RSP, 0x08
.set PERCPU_USER_RSP, 0x10
.set USER_DS, 0x23
.set USER_CS, 0x2b

.section .text
.global syscall_entry
.type syscall_entry, @function
syscall_entry:
    swapgs
    movq %rsp, %gs:PERCPU_USER_RSP
    movq %gs:PERCPU_KERNEL_RSP, %rsp
    pushq $USER_DS
    pushq %gs:PERCPU_USER_RSP
    pushq %r11                  // user RFLAGS
    pushq $USER_CS
    pushq %rcx                  // user RIP
    pushq $0                    // error code slot
    pushq $0x80                 // "vector" slot: marks a syscall frame
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
    call syscall_dispatch       // returns with interrupts off; eax = sysret is safe
    test %eax, %eax
    jz 1f
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
    addq $16, %rsp              // vector + error
    movq (%rsp), %rcx           // RIP
    movq 16(%rsp), %r11         // RFLAGS
    movq 24(%rsp), %rsp         // user RSP (NMIs run on an IST stack, so this window is safe)
    swapgs
    sysretq
1:  jmp interrupt_return        // non-canonical or modified RIP: iretq can't be tricked
.size syscall_entry, . - syscall_entry

// [[noreturn]] void enter_user(rip, rsp, argc, argv): first entry to ring 3.
.global enter_user
.type enter_user, @function
enter_user:
    cli
    pushq $USER_DS
    pushq %rsi
    pushq $0x202                // IF set
    pushq $USER_CS
    pushq %rdi
    mov %rdx, %rdi              // argc
    mov %rcx, %rsi              // argv
    xor %eax, %eax
    xor %ebx, %ebx
    xor %ecx, %ecx
    xor %edx, %edx
    xor %ebp, %ebp
    xor %r8d, %r8d
    xor %r9d, %r9d
    xor %r10d, %r10d
    xor %r11d, %r11d
    xor %r12d, %r12d
    xor %r13d, %r13d
    xor %r14d, %r14d
    xor %r15d, %r15d
    swapgs
    iretq
.size enter_user, . - enter_user
