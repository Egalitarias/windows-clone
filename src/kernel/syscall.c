#include "syscall.h"
#include "gdt.h"
#include "paging.h"
#include "scheduler.h"

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
    done += n;
  }
  return (int64_t)done;
}

int64_t syscall_dispatch(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3,
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
    case SYS_GETPID:
      return (int64_t)g_current_pid;
    default:
      return SYSCALL_ENOSYS;
  }
}
