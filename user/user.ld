/* User programs: static, loaded at 0x400000, one PT_LOAD per permission
 * class so the kernel's W^X loader accepts them. */
ENTRY(_start)

PHDRS {
  text   PT_LOAD FLAGS(5);
  rodata PT_LOAD FLAGS(4);
  data   PT_LOAD FLAGS(6);
}

SECTIONS {
  . = 0x400000;
  .text : ALIGN(4096) { *(.text.start) *(.text .text.*) } :text
  . = ALIGN(4096);
  .rodata : ALIGN(4096) { *(.rodata .rodata.*) } :rodata
  .init_array : ALIGN(8) {
    __init_array_start = .;
    KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
    KEEP(*(.init_array))
    __init_array_end = .;
  } :rodata
  . = ALIGN(4096);
  .data : ALIGN(4096) { *(.data .data.*) } :data
  .bss : { *(.bss .bss.*) *(COMMON) } :data
  /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame .eh_frame_hdr) }
}
