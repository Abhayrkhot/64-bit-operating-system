/* Shale kernel: higher half, one PT_LOAD per permission class so the loader
 * (and later the kernel itself) can map text R-X, rodata R--, data RW-. */
ENTRY(_start)

PHDRS {
  text   PT_LOAD FLAGS(5);
  rodata PT_LOAD FLAGS(4);
  data   PT_LOAD FLAGS(6);
}

SECTIONS {
  . = 0xFFFFFFFF80000000;
  __kernel_start = .;

  .text : ALIGN(4096) {
    __text_start = .;
    *(.text.entry)
    *(.text .text.*)
  } :text
  . = ALIGN(4096);
  __text_end = .;

  .rodata : ALIGN(4096) {
    __rodata_start = .;
    *(.rodata .rodata.*)
  } :rodata
  .ktests : ALIGN(8) {
    __ktests_start = .;
    KEEP(*(.ktests))
    __ktests_end = .;
  } :rodata
  .init_array : ALIGN(8) {
    __init_array_start = .;
    KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
    KEEP(*(.init_array))
    __init_array_end = .;
  } :rodata
  . = ALIGN(4096);
  __rodata_end = .;

  .data : ALIGN(4096) {
    __data_start = .;
    *(.data .data.*)
  } :data
  .bss : ALIGN(64) {
    __bss_start = .;
    *(.bss .bss.*)
    *(COMMON)
    __bss_end = .;
  } :data
  . = ALIGN(4096);
  __kernel_end = .;

  /DISCARD/ : {
    *(.comment)
    *(.note .note.*)
    *(.eh_frame .eh_frame_hdr)
  }
}
