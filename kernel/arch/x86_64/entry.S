// Kernel entry from the UEFI loader: RDI = BootInfo (HHDM address),
// interrupts off, loader page tables live. Switch to the boot stack and call
// kmain. The zeroed return address ends frame-pointer backtraces here.

.section .text.entry, "ax"
.global _start
.type _start, @function
_start:
    cli
    cld
    lea boot_stack_top(%rip), %rsp
    xor %ebp, %ebp
    push $0
    jmp kmain
.size _start, . - _start

.section .bss
.align 4096
.global boot_stack_bottom
boot_stack_bottom:
    .skip 65536
.global boot_stack_top
boot_stack_top:
