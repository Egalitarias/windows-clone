#ifndef KERNEL_PROCESS_H
#define KERNEL_PROCESS_H

#include "console.h"

// User program loader for flat binaries (see src/user/user.ld).
//
// Layout of a process (all in the lower half, mapped U/S):
//   PROCESS_LOAD_BASE            image (.text/.rodata/.data), then zeroed pages for
//                                .bss and a small heap (PROCESS_EXTRA_PAGES)
//   PROCESS_STACK_TOP - 16 KiB   stack (grows down); the page below is left
//                                unmapped as a guard page
//
// Limitation until per-process address spaces exist (phase 2): every process uses
// the same virtual addresses, so only one process can be loaded at a time.

#define PROCESS_LOAD_BASE    0x00007F0000001000ULL   // keep in sync with src/user/user.ld
#define PROCESS_STACK_TOP    0x00007F0000100000ULL
#define PROCESS_STACK_PAGES  4U
#define PROCESS_EXTRA_PAGES  4U
#define PROCESS_MAX_IMAGE    (512ULL * 1024ULL)
#define PROCESS_KSTACK_SIZE  16384U

#define PROCESS_OK            0
#define PROCESS_ERR_BUSY     (-1)   // another process is still loaded
#define PROCESS_ERR_NOFILE   (-2)   // path not found on any volume
#define PROCESS_ERR_TOOBIG   (-3)
#define PROCESS_ERR_NOMEM    (-4)
#define PROCESS_ERR_READ     (-5)
#define PROCESS_ERR_EMPTY    (-6)

typedef struct {
  int loaded;
  uint64_t pid;
  uint64_t entry;
  uint64_t image_phys;
  uint64_t image_pages;       // image + extra pages
  uint64_t stack_phys;
  void *kstack_alloc;         // kmalloc'd block holding the kernel stack
  uint64_t kstack_top;
  uint64_t saved_rsp;         // kernel context to return to when the program exits
  uint64_t exit_code;
} process_t;

// Load the flat binary at `path` (like "/APPS/HELLO.BIN", searched on every
// mounted volume) into a new process. Returns PROCESS_OK or a PROCESS_ERR_* code.
int process_create(const char *path, process_t *p);

// Run the program to completion. Blocks until it calls exit(); the exit code is
// stored in p->exit_code and returned.
uint64_t process_run(process_t *p);

// Unmap and free everything process_create allocated.
void process_destroy(process_t *p);

#ifdef KERNEL_USER_SELFTEST
void process_selftest(void);   // load and run /APPS/HELLO.BIN twice, then return
#endif

#endif // KERNEL_PROCESS_H
