.text
.align 16

.global kernel_image_start
kernel_image_start:
  .ascii "SPOSKRNL"                                  # Magic[8]
  .long 1                                            # AbiVersion
  .long kernel_header_end - kernel_image_start       # HeaderSize
  .quad _asm_kernel_entry - kernel_image_start       # EntryOffset
  .quad 0                                            # ImageSize (patched post-build)
  .quad 0                                            # Flags
  .quad __bss_size                                   # BssSize (linker symbol)

kernel_header_end:

# Trampoline: bootloader jumps here using Microsoft x64 ABI (RCX = KERNEL_BOOT_INFO*).
# Convert to SysV ABI for the C kernel_entry (RDI = first arg).
.global _asm_kernel_entry
_asm_kernel_entry:
  push %rbp
  mov  %rsp, %rbp
  mov  %rcx, %rdi          # ms_abi -> sysv_abi first argument register
  and  $~15, %rsp          # 16-byte align stack before C call
  call kernel_entry        # defined in main.c
  # kernel_entry is noreturn; hang here if it ever returns
1:
  hlt
  jmp 1b
