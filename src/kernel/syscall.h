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
#define SYS_READ    4   // read(fd, buf, len) -> bytes read; fd 0 is the keyboard, blocks until input (0 = end of input)

#define SYS_SLEEP   5   // sleep(ms) -> ms actually slept (>= ms); other tasks run meanwhile
#define SYS_OPEN    6   // open(path, flags) -> fd (>= 3); flags 0 = read file or directory, 1 = write (create/truncate on close)
#define SYS_CLOSE   7   // close(fd) -> 0; a write-opened file is stored now (-EIO if that fails)
#define SYS_WIN_CREATE 9    // win_create(title, w, h) -> window id; w x h is the client area (see syswin.h)
#define SYS_WIN_CLOSE  10   // win_close(id)
#define SYS_WIN_BLIT   11   // win_blit(id, syswin_op_t *)   copy pixels into the window
#define SYS_WIN_FILL   12   // win_fill(id, syswin_op_t *)   fill a rectangle
#define SYS_WIN_TEXT   13   // win_text(id, syswin_op_t *)   draw text (8x8 font)
#define SYS_WIN_EVENT  14   // win_event(id, syswin_event_t *, block) -> 1 event stored, 0 none
#define SYS_READDIR 8   // readdir(fd, sysfile_dirent_t *out) -> 1 entry stored, 0 at the end
                        // read(fd, ...) and write(fd, ...) also take file descriptors (see sysfile.h)

#define SYSCALL_SLEEP_MAX_MS 3600000ULL   // one hour

#define SYSCALL_ENOENT  (-2)
#define SYSCALL_EIO     (-5)
#define SYSCALL_ENOMEM  (-12)
#define SYSCALL_EACCES  (-13)
#define SYSCALL_ENODEV  (-19)
#define SYSCALL_EFAULT  (-14)
#define SYSCALL_ENOTDIR (-20)
#define SYSCALL_EISDIR  (-21)
#define SYSCALL_EBADF   (-9)
#define SYSCALL_EINVAL  (-22)
#define SYSCALL_EMFILE  (-24)
#define SYSCALL_EFBIG   (-27)
#define SYSCALL_ENAMETOOLONG (-36)
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
