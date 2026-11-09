#include "syscall.h"
#include "gdt.h"
#include "paging.h"
#include "scheduler.h"
#include "timer.h"
#include "process.h"
#include "sysfile.h"
#include "syswin.h"

#define MSR_EFER   0xC0000080U
#define MSR_STAR   0xC0000081U
#define MSR_LSTAR  0xC0000082U
#define MSR_SFMASK 0xC0000084U

#define EFER_SCE   (1ULL << 0)

// SFMASK: clear IF (no interrupts while on the user->kernel hand-off), DF and TF.
#define RFLAGS_TF  (1ULL << 8)
#define RFLAGS_IF  (1ULL << 9)
#define RFLAGS_DF  (1ULL << 10)

extern void syscall_entry(void);

static uint64_t g_current_pid = 1;
static syscall_exit_handler_t g_exit_handler = 0;

static uint64_t rdmsr(uint32_t msr) {
  uint32_t lo;
  uint32_t hi;

  __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
  return ((uint64_t)hi << 32) | lo;
}

static void wrmsr(uint32_t msr, uint64_t value) {
  __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

void syscall_init(void) {
  // STAR[47:32]  = kernel CS base: SYSCALL loads CS = 0x08, SS = 0x10.
  // STAR[63:48]  = 0x10: a 64-bit SYSRET loads SS = 0x10+8 | 3 = 0x1B and
  //                CS = 0x10+16 | 3 = 0x23 (the GDT order is chosen for this).
  uint64_t star = ((uint64_t)0x10 << 48) | ((uint64_t)GDT_KERNEL_CS << 32);

  wrmsr(MSR_STAR, star);
  wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
  wrmsr(MSR_SFMASK, RFLAGS_TF | RFLAGS_IF | RFLAGS_DF);
  wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
}

void syscall_set_current_pid(uint64_t pid) {
  g_current_pid = pid;
}

void syscall_set_exit_handler(syscall_exit_handler_t handler) {
  g_exit_handler = handler;
}

static int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len) {
  char chunk[128];
  uint64_t done = 0;

  if (fd >= SYSFILE_FIRST_FD) {
    return sysfile_write(fd, buf, len);
  }
  if (fd != 1 && fd != 2) {
    return SYSCALL_EBADF;
  }
  if (len > SYSCALL_WRITE_MAX) {
    return SYSCALL_EINVAL;
  }
  // Never touch a user pointer unless every page of it is mapped for ring 3.
  if (!paging_user_range_ok(buf, len)) {
    return SYSCALL_EFAULT;
  }

  while (done < len) {
    uint64_t n = len - done;
    uint64_t i;
    const char *src = (const char *)(uintptr_t)(buf + done);

    if (n > sizeof(chunk) - 1) {
      n = sizeof(chunk) - 1;
    }
    for (i = 0; i < n; ++i) {
      chunk[i] = src[i];
    }
    chunk[n] = 0;
    console_write_string(chunk);
    process_output_write(chunk, n);
    done += n;
  }
  return (int64_t)done;
}

static int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t len) {
  uint64_t n;

  if (fd >= SYSFILE_FIRST_FD) {
    return sysfile_read(fd, buf, len);
  }
  if (fd != 0) {
    return SYSCALL_EBADF;
  }
  if (len > SYSCALL_WRITE_MAX) {
    return SYSCALL_EINVAL;
  }
  if (len == 0) {
    return 0;
  }
  if (!paging_user_range_ok(buf, len)) {
    return SYSCALL_EFAULT;
  }
  // Block until there is input. Interrupts are off inside a syscall, so open
  // them for the wait exactly as SYS_YIELD does. Without a scheduler nothing
  // could ever supply input, so report end of input instead of hanging.
  for (;;) {
    n = process_input_read((char *)(uintptr_t)buf, len);
    if (n > 0) {
      scheduler_wait_end();
      return (int64_t)n;
    }
    if (process_input_eof() || !scheduler_is_running()) {
      scheduler_wait_end();
      return 0;
    }
    // Leave the run queue until input arrives (process_input_push wakes us).
    // Interrupts are off, so nothing can push between the check and the block.
    scheduler_block_current(TASK_WAIT_INPUT, 0);
    __asm__ volatile("sti; hlt; cli" : : : "memory");
  }
}

static int64_t sys_sleep(uint64_t ms) {
  uint64_t start;
  uint64_t now;

  if (ms > SYSCALL_SLEEP_MAX_MS) {
    return SYSCALL_EINVAL;
  }
  start = timer_get_ms();
  // Wait for the deadline with interrupts opened between checks, like SYS_YIELD:
  // each tick may switch to another task, and the CPU halts when none is runnable.
  // Without a running scheduler the tick still advances, so the loop ends too.
  for (;;) {
    now = timer_get_ms();
    if (now - start >= ms) {
      scheduler_wait_end();
      return (int64_t)(now - start);
    }
    // Leave the run queue; the timer tick wakes us at the deadline. Without a
    // running scheduler this is a plain halt until the tick advances.
    if (scheduler_is_running()) {
      scheduler_block_current(TASK_WAIT_SLEEP, start + ms);
    }
    __asm__ volatile("sti; hlt; cli" : : : "memory");
  }
}

static int64_t syscall_dispatch_inner(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5);

// Every syscall runs between enter and leave, so a ring-0 fault in the middle can
// be blamed on (and kill) just the calling process. See process_handle_kernel_fault.
int64_t syscall_dispatch(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5) {
  int64_t rc;

  process_syscall_enter();
  rc = syscall_dispatch_inner(nr, a1, a2, a3, a4, a5);
  process_syscall_leave();
  return rc;
}

static int64_t syscall_dispatch_inner(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  switch (nr) {
    case SYS_EXIT:
      if (g_exit_handler != 0) {
        g_exit_handler(a1);
      } else {
        console_printf("[syscall] exit(%llu) with no handler, halting\r\n", a1);
      }
      __asm__ volatile("cli");
      for (;;) {
        __asm__ volatile("hlt");
      }
    case SYS_WRITE:
      return sys_write(a1, a2, a3);
    case SYS_YIELD:
      // Give up the CPU until the next timer tick, which preempts to the next
      // ready task. Interrupts are off inside a syscall, so open them for the
      // wait; the kernel stack and the saved user state stay valid meanwhile.
      // Without a running scheduler there is nobody to yield to (and no tick).
      if (scheduler_is_running()) {
        __asm__ volatile("sti; hlt; cli" : : : "memory");
      }
      return 0;
    case SYS_SLEEP:
      return sys_sleep(a1);
    case SYS_READ:
      return sys_read(a1, a2, a3);
    case SYS_OPEN:
      return sysfile_open(a1, a2);
    case SYS_CLOSE:
      return sysfile_close(a1);
    case SYS_READDIR:
      return sysfile_readdir(a1, a2);
    case SYS_WIN_CREATE:
      return syswin_create(a1, a2, a3);
    case SYS_WIN_CLOSE:
      return syswin_close(a1);
    case SYS_WIN_BLIT:
      return syswin_blit(a1, a2);
    case SYS_WIN_FILL:
      return syswin_fill(a1, a2);
    case SYS_WIN_TEXT:
      return syswin_text(a1, a2);
    case SYS_WIN_EVENT:
      return syswin_event(a1, a2, a3);
    case SYS_GETARG: {
      const char *arg = process_current_arg();
      uint64_t n = 0;
      uint64_t i;

      while (arg[n] != 0) {
        ++n;
      }
      if (a2 == 0 || a2 > SYSCALL_WRITE_MAX) {
        return SYSCALL_EINVAL;
      }
      if (!paging_user_range_ok(a1, a2)) {
        return SYSCALL_EFAULT;
      }
      for (i = 0; i + 1 < a2 && i < n; ++i) {
        ((char *)(uintptr_t)a1)[i] = arg[i];
      }
      ((char *)(uintptr_t)a1)[i] = 0;
      return (int64_t)n;
    }
    case SYS_WIN_TITLE:
      return syswin_title(a1, a2);
    case SYS_GETPID: {
      uint64_t pid = process_current_pid();
      return (int64_t)(pid != 0 ? pid : g_current_pid);
    }
#if defined(KERNEL_USER_SELFTEST) && KERNEL_USER_SELFTEST == 9
    case SYS_TEST_KFAULT:
      // Self-test only: fault inside the kernel on purpose. a1 = 1 holds the
      // scheduler's preempt lock first, like the FAT driver does.
      if (a1 == 1) {
        scheduler_preempt_disable();
      }
      return *(volatile int64_t *)0;
#endif
    default:
      return SYSCALL_ENOSYS;
  }
}
