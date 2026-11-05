#ifndef KERNEL_USERMODE_H
#define KERNEL_USERMODE_H

#include "console.h"

// Switch to ring 3 with an iretq frame: SS=GDT_USER_DS, CS=GDT_USER_CS,
// RFLAGS=IF, RIP=entry, RSP=user_rsp. Does not return. TSS.rsp0 must already
// point at a valid kernel stack (see gdt_set_kernel_stack).
__attribute__((noreturn)) void user_enter(uint64_t entry, uint64_t user_rsp);

// Run a user program to completion. Saves the kernel's callee-saved registers,
// flags and RSP (into *saved_rsp), then enters ring 3 at `entry` with the given
// user stack. Returns the value passed to user_return() when the program exits.
uint64_t user_run(uint64_t entry, uint64_t user_rsp, uint64_t *saved_rsp);

// Resume the kernel context saved by user_run so that it returns `code`. Call it
// on the process's kernel stack (e.g. from the exit syscall). Never returns.
__attribute__((noreturn)) void user_return(uint64_t saved_rsp, uint64_t code);

#if defined(KERNEL_USER_SELFTEST) && (KERNEL_USER_SELFTEST == 1 || KERNEL_USER_SELFTEST == 2)
// Map a one-page user program plus stack and enter it. The program executes
// the privileged instruction `hlt`, so the expected result is a #GP exception
// reported with CS=0x23 (ring 3).
__attribute__((noreturn)) void usermode_selftest(void);
#endif

#endif // KERNEL_USERMODE_H
