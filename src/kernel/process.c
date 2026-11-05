#include "process.h"
#include "fs.h"
#include "gdt.h"
#include "heap.h"
#include "paging.h"
#include "pmm.h"
#include "scheduler.h"
#include "string.h"
#include "syscall.h"
#include "usermode.h"
#include "desktop.h"

static process_t *g_current = 0;
static volatile int g_busy = 0;     // a process owns the (single) user address space
static uint64_t g_next_pid = 1;

// Atomically claim the user address space. Tasks are preempted by the timer at
// any point, so a plain check-then-set would let two loaders in at once.
static int claim_address_space(void) {
  uint64_t flags;
  int got = 0;

  __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
  if (!g_busy) {
    g_busy = 1;
    got = 1;
  }
  if ((flags & 0x200ULL) != 0) {
    __asm__ volatile("sti" : : : "memory");
  }
  return got;
}

// Resolve "/DIR/NAME" on one volume, one component at a time.
static int lookup_path(fat_volume_t *v, const char *path, fat_dirent_t *out) {
  uint32_t dir = v->root_cluster;
  char part[FAT_NAME_MAX];

  while (*path == '/') {
    ++path;
  }
  if (*path == 0) {
    return 0;
  }
  for (;;) {
    size_t n = 0;
    while (*path != 0 && *path != '/') {
      if (n + 1 >= sizeof(part)) {
        return 0;
      }
      part[n++] = *path++;
    }
    part[n] = 0;
    while (*path == '/') {
      ++path;
    }
    if (!fat_lookup(v, dir, part, out)) {
      return 0;
    }
    if (*path == 0) {
      return (out->attr & FAT_ATTR_DIR) == 0;
    }
    if ((out->attr & FAT_ATTR_DIR) == 0) {
      return 0;
    }
    dir = out->first_cluster;
  }
}

static int find_file(const char *path, fat_volume_t **vol, fat_dirent_t *ent) {
  int i;

  for (i = 0; i < fs_volume_count(); ++i) {
    fat_volume_t *v = fs_volume(i);
    if (v != 0 && lookup_path(v, path, ent)) {
      *vol = v;
      return 1;
    }
  }
  return 0;
}

static void zero_pages(uint64_t phys, uint64_t pages) {
  memset((void *)(uintptr_t)phys, 0, (size_t)(pages * 4096ULL));
}

static void free_pages(uint64_t phys, uint64_t pages) {
  uint64_t i;

  for (i = 0; i < pages; ++i) {
    pmm_free_page(phys + i * 4096ULL);
  }
}

int process_create(const char *path, process_t *p) {
  fat_volume_t *vol = 0;
  fat_dirent_t ent;
  uint64_t image_bytes;
  uint32_t got = 0;
  uint64_t stack_virt = PROCESS_STACK_TOP - (uint64_t)PROCESS_STACK_PAGES * 4096ULL;
  uint8_t *kstack;

  if (!claim_address_space()) {
    return PROCESS_ERR_BUSY;  // same virtual addresses for everyone until phase 2
  }
  memset(p, 0, sizeof(*p));

  if (!find_file(path, &vol, &ent)) {
    g_busy = 0;
    return PROCESS_ERR_NOFILE;
  }
  if (ent.size == 0) {
    g_busy = 0;
    return PROCESS_ERR_EMPTY;
  }
  if (ent.size > PROCESS_MAX_IMAGE) {
    g_busy = 0;
    return PROCESS_ERR_TOOBIG;
  }
  image_bytes = ent.size;
  p->image_pages = (image_bytes + 4095ULL) / 4096ULL + PROCESS_EXTRA_PAGES;

  p->image_phys = pmm_alloc_pages(p->image_pages);
  if (p->image_phys == 0) {
    g_busy = 0;
    return PROCESS_ERR_NOMEM;
  }
  p->stack_phys = pmm_alloc_pages(PROCESS_STACK_PAGES);
  if (p->stack_phys == 0) {
    free_pages(p->image_phys, p->image_pages);
    g_busy = 0;
    return PROCESS_ERR_NOMEM;
  }
  p->kstack_alloc = kmalloc(PROCESS_KSTACK_SIZE + 16U);
  if (p->kstack_alloc == 0) {
    free_pages(p->stack_phys, PROCESS_STACK_PAGES);
    free_pages(p->image_phys, p->image_pages);
    g_busy = 0;
    return PROCESS_ERR_NOMEM;
  }

  // The kernel fills the pages through the identity map, so make sure it exists.
  if (!paging_map_range(p->image_phys, p->image_pages * 4096ULL) ||
      !paging_map_range(p->stack_phys, (uint64_t)PROCESS_STACK_PAGES * 4096ULL)) {
    kfree(p->kstack_alloc);
    free_pages(p->stack_phys, PROCESS_STACK_PAGES);
    free_pages(p->image_phys, p->image_pages);
    g_busy = 0;
    return PROCESS_ERR_NOMEM;
  }
  zero_pages(p->image_phys, p->image_pages);
  zero_pages(p->stack_phys, PROCESS_STACK_PAGES);

  if (!fat_read_file(vol, &ent, (void *)(uintptr_t)p->image_phys, (uint32_t)image_bytes, &got) ||
      got != image_bytes) {
    kfree(p->kstack_alloc);
    free_pages(p->stack_phys, PROCESS_STACK_PAGES);
    free_pages(p->image_phys, p->image_pages);
    g_busy = 0;
    return PROCESS_ERR_READ;
  }

  if (!paging_map_user(PROCESS_LOAD_BASE, p->image_phys, p->image_pages * 4096ULL, 1) ||
      !paging_map_user(stack_virt, p->stack_phys, (uint64_t)PROCESS_STACK_PAGES * 4096ULL, 1)) {
    paging_unmap_user(PROCESS_LOAD_BASE, p->image_pages * 4096ULL);
    paging_unmap_user(stack_virt, (uint64_t)PROCESS_STACK_PAGES * 4096ULL);
    kfree(p->kstack_alloc);
    free_pages(p->stack_phys, PROCESS_STACK_PAGES);
    free_pages(p->image_phys, p->image_pages);
    g_busy = 0;
    return PROCESS_ERR_NOMEM;
  }

  kstack = (uint8_t *)(((uintptr_t)p->kstack_alloc + 15U) & ~(uintptr_t)15U);
  p->kstack_top = (uint64_t)(uintptr_t)(kstack + PROCESS_KSTACK_SIZE);
  p->entry = PROCESS_LOAD_BASE;
  {
    // Under the scheduler the pid is the id of the task running the program.
    TCB *self = scheduler_current_tcb();
    p->pid = (self != 0) ? (uint64_t)self->task_id : g_next_pid++;
  }
  p->loaded = 1;
  g_current = p;
  return PROCESS_OK;
}

// Called by the exit syscall on the process's kernel stack: jump back into
// process_run's caller. Never returns.
static void process_exit_handler(uint64_t code) {
  process_t *p = g_current;

  p->exit_code = code;
  user_return(p->saved_rsp, code);
}

uint64_t process_run(process_t *p) {
  uint64_t code;

  if (p == 0 || !p->loaded) {
    return (uint64_t)-1;
  }
  gdt_set_kernel_stack(p->kstack_top);
  syscall_set_current_pid(p->pid);
  syscall_set_exit_handler(process_exit_handler);
  // The program starts as if called: RSP is 8 mod 16 at function entry.
  p->running = 1;
  code = user_run(p->entry, PROCESS_STACK_TOP - 8ULL, &p->saved_rsp);
  p->running = 0;
  p->exit_code = code;
  return code;
}

void process_destroy(process_t *p) {
  if (p == 0 || !p->loaded) {
    return;
  }
  paging_unmap_user(PROCESS_LOAD_BASE, p->image_pages * 4096ULL);
  paging_unmap_user(PROCESS_STACK_TOP - (uint64_t)PROCESS_STACK_PAGES * 4096ULL,
                    (uint64_t)PROCESS_STACK_PAGES * 4096ULL);
  free_pages(p->image_phys, p->image_pages);
  free_pages(p->stack_phys, PROCESS_STACK_PAGES);
  kfree(p->kstack_alloc);
  p->loaded = 0;
  if (g_current == p) {
    g_current = 0;
    syscall_set_exit_handler(0);
  }
  g_busy = 0;
}

int process_handle_user_fault(uint64_t vector, uint64_t error_code, uint64_t rip, uint64_t fault_addr) {
  process_t *p = g_current;

  if (p == 0 || !p->running) {
    return 0;
  }
  console_printf("[PROC] pid %llu killed: exception %llu (error 0x%llx) at rip 0x%llx, address 0x%llx\r\n",
                 p->pid, vector, error_code, rip, fault_addr);
  p->exit_code = PROCESS_EXIT_FAULT_BASE + vector;
  // We are on the process's kernel stack; unwind to the code that called process_run.
  user_return(p->saved_rsp, p->exit_code);
}

// ---- Spawning programs as scheduler tasks -----------------------------------

#define SPAWN_MAX 8

typedef struct {
  int used;
  volatile int done;
  int error;
  uint64_t exit_code;
  char path[64];
  char name[40];
} spawn_t;

static spawn_t g_spawns[SPAWN_MAX];

static void wait_one_tick(void) {
  __asm__ volatile("sti; hlt" : : : "memory");
}

static int spawn_task(void *arg) {
  spawn_t *s = (spawn_t *)arg;
  process_t p;
  int rc;

  // Only one process can own the user address space; wait for our turn.
  for (;;) {
    rc = process_create(s->path, &p);
    if (rc != PROCESS_ERR_BUSY) {
      break;
    }
    wait_one_tick();
  }
  if (rc != PROCESS_OK) {
    s->error = rc;
    s->exit_code = (uint64_t)-1;
    s->done = 1;
    return 0;
  }

  s->exit_code = process_run(&p);
  {
    TCB *self = scheduler_current_tcb();
    console_printf("[PROC] pid %llu (%s) exited with %llu after %u context switches\r\n",
                   p.pid, s->path, s->exit_code, self != 0 ? self->context_switches : 0U);
  }
  process_destroy(&p);
  s->error = PROCESS_OK;
  s->done = 1;
  return 0;
}

int process_spawn(const char *path) {
  int i;
  int handle = -1;
  uint64_t flags;

  __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
  for (i = 0; i < SPAWN_MAX; ++i) {
    if (!g_spawns[i].used) {
      g_spawns[i].used = 1;
      handle = i;
      break;
    }
  }
  if ((flags & 0x200ULL) != 0) {
    __asm__ volatile("sti" : : : "memory");
  }
  if (handle < 0) {
    return PROCESS_ERR_NOMEM;
  }

  {
    spawn_t *s = &g_spawns[handle];
    size_t n = str_copy(s->name, "user:", sizeof(s->name));
    str_copy(s->name + n, path, sizeof(s->name) - n);
    str_copy(s->path, path, sizeof(s->path));
    s->done = 0;
    s->error = PROCESS_OK;
    s->exit_code = 0;
    if (!scheduler_add_task(s->name, spawn_task, s)) {
      s->used = 0;
      return PROCESS_ERR_NOMEM;
    }
  }
  return handle;
}

// ---- Program output ----------------------------------------------------------
// What user programs write(2) is also kept in a ring buffer so the desktop can show
// it. Readers keep their own cursor (total bytes seen); torn reads are harmless.

#define OUT_RING 4096U
static char g_out_ring[OUT_RING];
static volatile uint64_t g_out_total = 0;

void process_output_write(const char *data, uint64_t len) {
  uint64_t i;
  for (i = 0; i < len; ++i) {
    g_out_ring[g_out_total % OUT_RING] = data[i];
    g_out_total = g_out_total + 1;
  }
}

uint64_t process_output_read(uint64_t *cursor, char *buf, uint64_t max) {
  uint64_t total = g_out_total;
  uint64_t n = 0;

  if (total - *cursor > OUT_RING) {
    *cursor = total - OUT_RING;       // fell behind: skip what was overwritten
  }
  while (*cursor < total && n < max) {
    buf[n++] = g_out_ring[*cursor % OUT_RING];
    *cursor = *cursor + 1;
  }
  return n;
}

int process_poll(int handle, uint64_t *exit_code) {
  spawn_t *s;
  int error;

  if (handle < 0 || handle >= SPAWN_MAX || !g_spawns[handle].used) {
    return PROCESS_ERR_NOFILE;
  }
  s = &g_spawns[handle];
  if (!s->done) {
    return 1;                          // still running
  }
  if (exit_code != 0) {
    *exit_code = s->exit_code;
  }
  error = s->error;
  s->used = 0;
  return error;                        // PROCESS_OK or a load error; the handle is now free
}

int process_wait(int handle, uint64_t *exit_code) {
  spawn_t *s;
  int error;

  if (handle < 0 || handle >= SPAWN_MAX || !g_spawns[handle].used) {
    return PROCESS_ERR_NOFILE;
  }
  s = &g_spawns[handle];
  while (!s->done) {
    wait_one_tick();
  }
  if (exit_code != 0) {
    *exit_code = s->exit_code;
  }
  error = s->error;
  s->used = 0;
  return error;
}

#ifdef KERNEL_USER_SELFTEST
void process_selftest(void) {
  int round;
  int failures = 0;

  console_write_string("[PROC] loader self-test: /APPS/HELLO.BIN\r\n");
  fs_init();
  console_printf("[PROC] %d volume(s) mounted\r\n", fs_volume_count());

  // Twice, to prove that destroy releases everything and that a re-run works.
  for (round = 1; round <= 2; ++round) {
    process_t p;
    int rc = process_create("/APPS/HELLO.BIN", &p);
    uint64_t code;

    if (rc != PROCESS_OK) {
      console_printf("[PROC] FAIL round %d: process_create returned %d\r\n", round, rc);
      failures++;
      break;
    }
    console_printf("[PROC] round %d: pid %llu, %llu image pages\r\n", round, p.pid, p.image_pages);
    code = process_run(&p);
    console_printf("[PROC] round %d: program exited with %llu\r\n", round, code);
    if (code != 7) {
      failures++;
    }
    process_destroy(&p);
  }

  // A missing file must fail cleanly.
  {
    process_t p;
    int rc = process_create("/APPS/NOPE.BIN", &p);
    if (rc != PROCESS_ERR_NOFILE) {
      console_printf("[PROC] FAIL: missing file returned %d\r\n", rc);
      failures++;
    }
  }

  if (failures == 0) {
    console_write_string("[PROC] PASS: program loaded, ran, exited and was cleaned up\r\n");
  } else {
    console_printf("[PROC] FAIL: %d problem(s)\r\n", failures);
  }
}
#endif

#if defined(KERNEL_USER_SELFTEST) && KERNEL_USER_SELFTEST == 4
// Supervisor task for USER_SELFTEST=4. It spawns programs while the demo tasks
// are also running, so every check below happens under timer preemption.
static int expect(int ok, const char *what) {
  console_printf("[SUP]   %s %s\r\n", ok ? "ok  " : "FAIL", what);
  return ok ? 0 : 1;
}

static int supervisor_task(void *arg) {
  int failures = 0;
  int h_count, h_hello, h_crash, h_missing;
  uint64_t code = 0;
  int i;

  (void)arg;
  console_write_string("[SUP] queue four programs at once: count, hello, crash, missing\r\n");
  h_count = process_spawn("/APPS/COUNT.BIN");
  h_hello = process_spawn("/APPS/HELLO.BIN");
  h_crash = process_spawn("/APPS/CRASH.BIN");
  h_missing = process_spawn("/APPS/NOPE.BIN");
  failures += expect(h_count >= 0 && h_hello >= 0 && h_crash >= 0 && h_missing >= 0, "all four spawned");

  failures += expect(process_wait(h_count, &code) == PROCESS_OK && code == 9, "count ran (preempted) and exited 9");
  failures += expect(process_wait(h_hello, &code) == PROCESS_OK && code == 7, "hello exited 7");
  failures += expect(process_wait(h_crash, &code) == PROCESS_OK && code == PROCESS_EXIT_FAULT_BASE + 14,
                     "crash was killed by a page fault, kernel survived");
  failures += expect(process_wait(h_missing, &code) == PROCESS_ERR_NOFILE, "missing program reports NOFILE");

  // More launches than the scheduler has task slots: only works if slots of
  // finished tasks are reclaimed.
  console_write_string("[SUP] twelve sequential launches (slot reuse)\r\n");
  for (i = 0; i < 12; ++i) {
    int h = process_spawn("/APPS/HELLO.BIN");
    if (h < 0 || process_wait(h, &code) != PROCESS_OK || code != 7) {
      failures += expect(0, "sequential launch");
      break;
    }
  }
  failures += expect(i == 12, "all twelve sequential launches ran");

  if (failures == 0) {
    console_write_string("[SUP] PASS: scheduled processes, preemption, fault isolation, slot reuse\r\n");
  } else {
    console_printf("[SUP] FAIL: %d check(s) failed\r\n", failures);
  }
  return 0;
}

void process_scheduler_selftest(void) {
  fs_init();   // the desktop normally mounts the volumes; this test runs without it
  scheduler_add_task("supervisor", supervisor_task, 0);
}
#endif

#if defined(KERNEL_USER_SELFTEST) && KERNEL_USER_SELFTEST == 5
// USER_SELFTEST=5: the desktop is a normal task, so user programs must be able
// to run, be preempted and crash while it keeps running.
static int desk_expect(int ok, const char *what) {
  console_printf("[DESK]  %s %s\r\n", ok ? "ok  " : "FAIL", what);
  return ok ? 0 : 1;
}

static int desktop_supervisor_task(void *arg) {
  int failures = 0;
  uint64_t waited = 0;
  uint64_t before;
  uint64_t after;
  uint64_t code = 0;
  int h;
  int i;

  (void)arg;
  console_write_string("[DESK] waiting for the desktop task to come up\r\n");
  while (desktop_loop_count() < 3 && waited < 20000) {
    wait_one_tick();
    ++waited;
  }
  failures += desk_expect(desktop_loop_count() >= 3, "desktop task is running its main loop");

  // A busy program: the desktop must keep getting turns while it runs in ring 3.
  before = desktop_loop_count();
  h = process_spawn("/APPS/COUNT.BIN");
  failures += desk_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == 9, "count ran to completion");
  after = desktop_loop_count();
  console_printf("[DESK] desktop passes while count ran: %llu\r\n", after - before);
  failures += desk_expect(after - before >= 10, "desktop kept running while a program was busy");

  // A crashing program must not take the desktop with it.
  h = process_spawn("/APPS/CRASH.BIN");
  failures += desk_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == PROCESS_EXIT_FAULT_BASE + 14,
                          "crash was contained");
  before = desktop_loop_count();
  for (i = 0; i < 100; ++i) {
    wait_one_tick();
  }
  after = desktop_loop_count();
  failures += desk_expect(after > before, "desktop still alive after the crash");

  // And many launches in a row, with the desktop sharing the CPU the whole time.
  for (i = 0; i < 10; ++i) {
    h = process_spawn("/APPS/HELLO.BIN");
    if (h < 0 || process_wait(h, &code) != PROCESS_OK || code != 7) {
      break;
    }
  }
  failures += desk_expect(i == 10, "ten more launches ran");
  before = desktop_loop_count();
  for (i = 0; i < 100; ++i) {
    wait_one_tick();
  }
  failures += desk_expect(desktop_loop_count() > before, "desktop still alive at the end");

  if (failures == 0) {
    console_write_string("[DESK] PASS: desktop is a regular task and coexists with user programs\r\n");
  } else {
    console_printf("[DESK] FAIL: %d check(s) failed\r\n", failures);
  }
  return 0;
}

// Start the desktop right away (instead of from the idle callback, which only
// fires once every task has finished) and add the supervisor.
void process_desktop_selftest(void) {
  desktop_start();
  scheduler_add_task("desk-supervisor", desktop_supervisor_task, 0);
}
#endif
