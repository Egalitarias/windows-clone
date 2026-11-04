#ifndef KERNEL_USERMODE_H
#define KERNEL_USERMODE_H

#include "console.h"

// Switch to ring 3 with an iretq frame: SS=GDT_USER_DS, CS=GDT_USER_CS,
// RFLAGS=IF, RIP=entry, RSP=user_rsp. Does not return. TSS.rsp0 must already
// point at a valid kernel stack (see gdt_set_kernel_stack).
__attribute__((noreturn)) void user_enter(uint64_t entry, uint64_t user_rsp);

#ifdef KERNEL_USER_SELFTEST
// Map a one-page user program plus stack and enter it. The program executes
// the privileged instruction `hlt`, so the expected result is a #GP exception
// reported with CS=0x23 (ring 3).
__attribute__((noreturn)) void usermode_selftest(void);
#endif

#endif // KERNEL_USERMODE_H
