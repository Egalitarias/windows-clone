.section .text

// SYSCALL entry. The CPU has already done:
//   RCX = user RIP, R11 = user RFLAGS, CS/SS = kernel selectors (from STAR),
//   RFLAGS &= ~SFMASK (IF cleared). RSP is still the USER stack pointer.
//
// This kernel is single-CPU, so the user RSP and the kernel stack top live in
// plain globals instead of behind swapgs. g_kernel_rsp0 is kept in sync with
// TSS.rsp0 by gdt_set_kernel_stack().
//
// Registers on entry: RAX = number, RDI RSI RDX R10 R8 = arguments.

.globl syscall_entry
syscall_entry:
    mov %rsp, g_syscall_user_rsp(%rip)
    mov g_kernel_rsp0(%rip), %rsp

    pushq g_syscall_user_rsp(%rip)   // user RSP
    push %rcx                        // user RIP
    push %r11                        // user RFLAGS
    sub $8, %rsp                     // keep the stack 16-byte aligned for the call

    // Shuffle into the SysV C ABI: dispatch(nr, a1, a2, a3, a4, a5).
    mov %r8, %r9
    mov %r10, %r8
    mov %rdx, %rcx
    mov %rsi, %rdx
    mov %rdi, %rsi
    mov %rax, %rdi
    call syscall_dispatch            // result in RAX

    add $8, %rsp
    pop %r11                         // user RFLAGS (restores IF)
    pop %rcx                         // user RIP
    pop %rsp                         // user RSP
    sysretq

.section .data
.globl g_syscall_user_rsp
.balign 8
g_syscall_user_rsp:
    .quad 0
