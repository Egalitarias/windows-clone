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
// Every process has its own address space (a private PML4 sharing the kernel's
// mappings), so all of them use these same virtual addresses and any number can be
// loaded at once; the scheduler switches CR3 with the task.

#define PROCESS_LOAD_BASE    0x00007F0000001000ULL   // keep in sync with src/user/user.ld
#define PROCESS_STACK_TOP    0x00007F0000100000ULL
#define PROCESS_STACK_PAGES  4U
#define PROCESS_EXTRA_PAGES  4U
#define PROCESS_MAX_IMAGE    (512ULL * 1024ULL)
#define PROCESS_KSTACK_SIZE  16384U

#define PROCESS_OK            0
#define PROCESS_ERR_BUSY     (-1)   // unused now that each process has its own address space
#define PROCESS_ERR_NOFILE   (-2)   // path not found on any volume
#define PROCESS_ERR_TOOBIG   (-3)
#define PROCESS_ERR_NOMEM    (-4)
#define PROCESS_ERR_READ     (-5)
#define PROCESS_ERR_EMPTY    (-6)

// A program killed by a CPU exception in user mode reports this plus the vector
// as its exit code (0x10E = page fault, 0x10D = general protection fault).
#define PROCESS_EXIT_FAULT_BASE 0x100ULL

typedef struct {
  int loaded;
  uint64_t pid;
  uint64_t entry;
  uint64_t image_phys;
  uint64_t image_pages;       // image + extra pages
  uint64_t stack_phys;
  void *kstack_alloc;         // kmalloc'd block holding the kernel stack
  uint64_t kstack_top;
  uint64_t cr3;               // physical address of the process's PML4 (its own address space)
  uint64_t saved_rsp;         // kernel context to return to when the program exits
  uint64_t exit_code;
  void *io;                   // the program's console (set by process_spawn), or 0
  const char *arg;            // argument string given to process_spawn_arg, or 0
  void *files;                // open-file table (sysfile.c), or 0
  int running;                // inside process_run (user code or one of its syscalls)
} process_t;

// Load the flat binary at `path` (like "/APPS/HELLO.BIN", searched on every
// mounted volume) into a new process. Returns PROCESS_OK or a PROCESS_ERR_* code.
int process_create(const char *path, process_t *p);

// Run the program to completion. Blocks until it calls exit(); the exit code is
// stored in p->exit_code and returned.
uint64_t process_run(process_t *p);

// Unmap and free everything process_create allocated.
void process_destroy(process_t *p);

// Run a program as its own scheduler task. The task loads the file (waiting its
// turn while another process is loaded), runs it, frees it and records the exit
// code. Returns a handle for process_wait(), or a negative PROCESS_ERR_* code.
// The program is preemptible: timer ticks interrupt it in ring 3 like any task.
int process_spawn(const char *path);
// Same, with an argument string (at most PROCESS_ARG_MAX - 1 characters) the program
// reads with the getarg syscall - e.g. the file to open.
int process_spawn_arg(const char *path, const char *arg);
#define PROCESS_ARG_MAX 64

// Wait (from a scheduler task) until the spawned program has finished. Stores
// the exit code and returns PROCESS_OK, or the load error if it never started.
int process_wait(int handle, uint64_t *exit_code);

// Non-blocking check on a spawned program. Returns 1 while it is still running;
// otherwise behaves like process_wait (stores the exit code, releases the handle).
int process_poll(int handle, uint64_t *exit_code);

// Non-blocking: 1 once the spawned program has finished (its console is still
// readable until process_poll releases the handle).
int process_finished(int handle);

// Each spawned program has its own console, addressed by its spawn handle (valid
// until process_poll/process_wait releases it). Output: everything the program
// writes to stdout/stderr; process_output_read copies new bytes after *cursor (a
// running total, start at 0). Input: the GUI pushes characters (whole lines);
// SYS_READ drains them. process_input_close() marks end of input: once the queue is
// empty reads return 0. The syscall-side calls below act on the calling program.
void process_output_write(const char *data, uint64_t len);
uint64_t process_output_read(int handle, uint64_t *cursor, char *buf, uint64_t max);
void process_input_push(int handle, char c);
void process_input_close(int handle);
uint64_t process_input_read(char *buf, uint64_t max);   // never blocks; returns bytes copied
int process_input_eof(void);

// The open-file table slot of the process on this task (0 if there is none).
void **process_files_slot(void);

// The argument of the process on this task ("" if it has none).
const char *process_current_arg(void);

// Pid of the process running on this task, or 0 if the task is not a process.
uint64_t process_current_pid(void);

// Number of processes loaded right now, and the most that were ever loaded at once.
int process_live_count(void);
int process_live_max(void);

// Called for CPU exceptions that arrived from ring 3. If a process is running it
// is terminated with PROCESS_EXIT_FAULT_BASE + vector and control returns to the
// code that called process_run (this function does not return). Returns 0 if no
// process is running, so the caller can treat it as a kernel fault.
int process_handle_user_fault(uint64_t vector, uint64_t error_code, uint64_t rip, uint64_t fault_addr);

#ifdef KERNEL_USER_SELFTEST
void process_selftest(void);             // USER_SELFTEST=3: load and run /APPS/HELLO.BIN twice, then return
void process_scheduler_selftest(void);   // USER_SELFTEST=4: add a supervisor task that spawns programs
void process_desktop_selftest(void);     // USER_SELFTEST=5: same, with the desktop running as a task
void process_isolation_selftest(void);   // USER_SELFTEST=6: concurrent processes in separate address spaces
void process_file_selftest(void);        // USER_SELFTEST=7: file syscalls (FILES.BIN)
void process_window_selftest(void);      // USER_SELFTEST=8: window syscalls (WINTEST.BIN, WINEV.BIN)
#endif

#endif // KERNEL_PROCESS_H
