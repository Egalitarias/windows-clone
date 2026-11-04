#ifndef KERNEL_SYSCALL_H
#define KERNEL_SYSCALL_H

#include "console.h"

// Syscall numbers (passed in RAX). Arguments go in RDI, RSI, RDX, R10, R8
// (R9 is not forwarded yet). The result comes back in RAX; negative values are
// errors (-errno). SYSCALL clobbers RCX and R11.
#define SYS_EXIT    0   // exit(code)
#define SYS_WRITE   1   // write(fd, buf, len) -> bytes written; fd 1 and 2 go to the console
#define SYS_YIELD   2   // yield() -> 0
#define SYS_GETPID  3   // getpid() -> pid

#define SYSCALL_EFAULT  (-14)
#define SYSCALL_EBADF   (-9)
#define SYSCALL_EINVAL  (-22)
#define SYSCALL_ENOSYS  (-38)

#define SYSCALL_WRITE_MAX 4096ULL

// Enable SYSCALL/SYSRET: EFER.SCE, STAR, LSTAR, SFMASK. Call after gdt_init().
void syscall_init(void);

// Process id reported by SYS_GETPID until the process table exists (phase 4).
void syscall_set_current_pid(uint64_t pid);

// Called by the exit syscall. Set by the owner of the running user program; the
// default handler reports the code and halts.
typedef void (*syscall_exit_handler_t)(uint64_t code);
void syscall_set_exit_handler(syscall_exit_handler_t handler);

// C entry point called from syscall_entry (syscall_entry.S).
int64_t syscall_dispatch(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5);

#endif // KERNEL_SYSCALL_H
