#ifndef KERNEL_PROCESS_H
#define KERNEL_PROCESS_H

#include "console.h"
#include "tcb.h"

// User program loader: ELF64 executables and flat binaries (see src/user/user.ld).
//
// Layout of a process (all in the lower half, mapped U/S):
//   PROCESS_LOAD_BASE            image (.text/.rodata/.data), then zeroed pages for
//                                .bss and a small heap (PROCESS_EXTRA_PAGES)
//   PROCESS_STACK_TOP - 64 KiB   stack (grows down); the page below is left
//                                unmapped as a guard page
//
// Every process has its own address space (a private PML4 sharing the kernel's
// mappings), so all of them use these same virtual addresses and any number can be
// loaded at once; the scheduler switches CR3 with the task.

#define PROCESS_LOAD_BASE    0x00007F0000001000ULL   // keep in sync with src/user/user.ld
#define PROCESS_STACK_TOP    0x00007F0000100000ULL
#define PROCESS_STACK_PAGES  16U
#define PROCESS_EXTRA_PAGES  12U
#define PROCESS_MAX_IMAGE    (512ULL * 1024ULL)
#define PROCESS_KSTACK_SIZE  16384U

#define PROCESS_OK            0
#define PROCESS_ERR_BUSY     (-1)   // unused now that each process has its own address space
#define PROCESS_ERR_NOFILE   (-2)   // path not found on any volume
#define PROCESS_ERR_TOOBIG   (-3)
#define PROCESS_ERR_NOMEM    (-4)
#define PROCESS_ERR_READ     (-5)
#define PROCESS_ERR_EMPTY    (-6)
#define PROCESS_ERR_BADPE    (-8)   // a PE file the loader cannot use (not x64, bad headers, a DLL, ...)
#define PROCESS_ERR_NOSHIM   (-9)   // a PE program but /APPS/WIN32.DLL (the Win32 runtime) is missing
#define PROCESS_ERR_BADELF   (-7)   // an ELF file the loader cannot use (wrong type, bad segments)

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
  uint64_t initial_sp;        // user RSP at the entry point (0 = the default for ELF programs)
  int is_pe;                  // a Windows (PE) program: has a heap block and the Win32 DLL
  uint64_t heap_phys, heap_pages;   // PE programs: the heap at PE_HEAP_BASE (the Win32 runtime manages it)
  uint64_t pe_exe_base, pe_dll_base;
  uint64_t gs_base;           // PE programs: the thread block (TEB) GS points at
  uint64_t exit_code;
  void *io;                   // the program's console (set by process_spawn), or 0
  const char *arg;            // argument string given to process_spawn_arg, or 0
  void *files;                // open-file table (sysfile.c), or 0
  task_uctx_t fb_uctx;        // run state when the program is not run by a scheduler task (see tcb.h)
  // Threads (PE programs; see process_thread_create).
  uint64_t die_stub;          // WIN32.DLL's __winclone_die: a thread killed while in user mode is redirected there
  uint64_t tls_off, tls_len, tls_total;   // the program's TLS template: offset in the image, bytes, bytes with zero fill
  void *threads;              // the extra threads' records (kmalloc'd table), or 0
  int live_threads;           // extra thread tasks that have not finished
  void *main_tcb;             // the task running the main thread
  int exiting;                // the process is ending (exit() or a fault in some thread)
} process_t;

// Load the program (ELF or flat binary) at `path` (like "/APPS/HELLO.BIN", searched on every
// mounted volume) into a new process. Returns PROCESS_OK or a PROCESS_ERR_* code.
int process_create(const char *path, process_t *p);

// Where a PE program's heap lives, and how big it is.
#define PE_HEAP_BASE  0x00007F0000200000ULL
#define PE_HEAP_PAGES 512U
// The first pages of that block belong to the kernel: page 0 is the TEB (GS points at it, with
// the PEB and the TLS pointer array in the same page), the rest is the program's thread-local
// storage. The runtime's heap starts after them.
#define PE_RESERVED_PAGES 4U
#define PE_SHIM_PATH  "/APPS/WIN32.DLL"
// For SYS_PEINFO: the current process is a PE program: fills out[0..3] = heap base, heap
// size, program image base, Win32 DLL base and returns 1; 0 for any other program.
int process_pe_info(uint64_t out[4]);

// Threads (PE programs). Each extra thread is a scheduler task that runs in the program's address
// space with its own user stack, kernel stack and thread block (TEB); the program's main thread is
// the task that called process_run. thread_create(start_stub, fn, arg) starts `start_stub` in
// the new thread, which finds fn and arg at gs:[0x300] and gs:[0x308]; it returns the thread id or
// a negative errno. Ending the process (exit(), a fault, the main thread returning) ends every
// thread: the others are flagged, woken, and redirected to exit from user mode.
int64_t process_thread_create(uint64_t start_stub, uint64_t fn, uint64_t arg);
void process_thread_exit(uint64_t code);
// Nonzero when this thread has been asked to end: blocking syscalls then return early.
int process_kill_pending(void);

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
// Start a program nobody will wait for: its slot is released when it ends. Returns 0 or a
// negative PROCESS_ERR_* code (PROCESS_ERR_NOMEM when no slot or task is free). A path
// that does not load is only reported on the kernel console.
int process_spawn_detached(const char *path, const char *arg);
// 1 if `path` names a loadable file on any volume.
int process_program_exists(const char *path);
// 1 if a program started from `path` is still running (or starting). *pid gets its
// process id, 0 while it is still loading.
int process_spawn_active(const char *path, uint64_t *pid);
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

// Bracket every syscall (syscall_dispatch). While inside, a CPU exception in ring 0
// is a kernel fault caused by that process's request, not by a kernel-wide bug.
void process_syscall_enter(void);
void process_syscall_leave(void);

// Called for CPU exceptions that arrived in ring 0. If they happened inside a
// syscall of a running process, that process is killed with
// PROCESS_EXIT_FAULT_BASE + vector, the preempt lock the syscall may have held is
// released, and control returns to the code that called process_run (this function
// does not return). Returns 0 otherwise, so the caller treats it as a kernel crash.
int process_handle_kernel_fault(uint64_t vector, uint64_t error_code, uint64_t rip, uint64_t fault_addr);

#ifdef KERNEL_USER_SELFTEST
void process_selftest(void);            // USER_SELFTEST=3: load and run /APPS/HELLO.BIN twice, then return
void process_scheduler_selftest(void);   // USER_SELFTEST=4: add a supervisor task that spawns programs
void process_desktop_selftest(void);     // USER_SELFTEST=5: same, with the desktop running as a task
void process_isolation_selftest(void);   // USER_SELFTEST=6: concurrent processes in separate address spaces
void process_file_selftest(void);        // USER_SELFTEST=7: file syscalls (FILES.BIN)
void process_window_selftest(void);      // USER_SELFTEST=8: window syscalls (WINTEST.BIN, WINEV.BIN)
void process_pe_demo(void);               // USER_SELFTEST=11: leave the Windows demo programs open (for a screenshot)
void process_pe_selftest(void);          // USER_SELFTEST=10: Windows (PE) programs
void process_hardening_selftest(void);   // USER_SELFTEST=9: bad pointers, stack overflow, preemption
#endif

#endif // KERNEL_PROCESS_H
