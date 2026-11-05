/* Linker script for WinClone user programs: a flat binary loaded at a fixed
   user virtual address. The entry point is the first byte of the image.
   Keep USER_LOAD_BASE in sync with src/kernel/process.h. .bss is not stored in
   the file; the loader maps zeroed pages after the image. */
ENTRY(_start)

SECTIONS
{
  . = 0x00007F0000001000;

  .text   : { *(.text._start) *(.text .text.*) }
  .rodata : { *(.rodata .rodata.*) }
  .data   : { *(.data .data.*) }
  .bss    : { *(.bss .bss.*) *(COMMON) }

  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
