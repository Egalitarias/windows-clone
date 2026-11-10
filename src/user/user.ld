/* Linker script for WinClone user programs: an ELF64 executable at a fixed user
   virtual address (no relocation, no dynamic linking). The kernel's loader maps the
   PT_LOAD segments: code and constants read-only, data and .bss writable, each
   starting on its own page. Keep the base in sync with PROCESS_LOAD_BASE in
   src/kernel/process.h. .bss is not stored in the file; the loader zeroes it. */
ENTRY(_start)

PHDRS
{
  text PT_LOAD FILEHDR PHDRS FLAGS(5);   /* R+X: headers, code, constants */
  data PT_LOAD FLAGS(6);                 /* R+W: .data and .bss */
}

SECTIONS
{
  . = 0x00007F0000001000 + SIZEOF_HEADERS;

  .text   : { *(.text._start) *(.text .text.*) } :text
  .rodata : { *(.rodata .rodata.*) } :text

  . = ALIGN(4096);
  .data   : { *(.data .data.*) } :data
  .bss    : { *(.bss .bss.*) *(COMMON) } :data

  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
