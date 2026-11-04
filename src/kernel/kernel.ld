/* Linker script for UEFI kernel */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(kernel_entry)

SECTIONS
{
  . = 0x100000;

  .text : ALIGN(0x1000) {
    __kernel_image_start = .;
    __text_start = .;
    *(.text)
    *(.text.*)
    __text_end = .;
  }

  .rodata : ALIGN(0x1000) {
    __rodata_start = .;
    *(.rodata)
    *(.rodata.*)
    __rodata_end = .;
  }

  .data : ALIGN(0x1000) {
    __data_start = .;
    *(.data)
    *(.data.*)
    __data_end = .;
  }

  .bss : ALIGN(0x1000) {
    __bss_start = .;
    *(.bss)
    *(.bss.*)
    . = ALIGN(0x1000);
    __bss_end = .;
  }

  __bss_size = __bss_end - __bss_start;
}
