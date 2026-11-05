#include "process.h"
#include "fs.h"
#include "gdt.h"
#include "heap.h"
#include "paging.h"
#include "pmm.h"
#include "string.h"
#include "syscall.h"
#include "usermode.h"

static process_t *g_current = 0;
static uint64_t g_next_pid = 1;

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

  if (g_current != 0) {
    return PROCESS_ERR_BUSY;  // same virtual addresses for everyone until phase 2
  }
  memset(p, 0, sizeof(*p));

  if (!find_file(path, &vol, &ent)) {
    return PROCESS_ERR_NOFILE;
  }
  if (ent.size == 0) {
    return PROCESS_ERR_EMPTY;
  }
  if (ent.size > PROCESS_MAX_IMAGE) {
    return PROCESS_ERR_TOOBIG;
  }
  image_bytes = ent.size;
  p->image_pages = (image_bytes + 4095ULL) / 4096ULL + PROCESS_EXTRA_PAGES;

  p->image_phys = pmm_alloc_pages(p->image_pages);
  if (p->image_phys == 0) {
    return PROCESS_ERR_NOMEM;
  }
  p->stack_phys = pmm_alloc_pages(PROCESS_STACK_PAGES);
  if (p->stack_phys == 0) {
    free_pages(p->image_phys, p->image_pages);
    return PROCESS_ERR_NOMEM;
  }
  p->kstack_alloc = kmalloc(PROCESS_KSTACK_SIZE + 16U);
  if (p->kstack_alloc == 0) {
    free_pages(p->stack_phys, PROCESS_STACK_PAGES);
    free_pages(p->image_phys, p->image_pages);
    return PROCESS_ERR_NOMEM;
  }

  // The kernel fills the pages through the identity map, so make sure it exists.
  if (!paging_map_range(p->image_phys, p->image_pages * 4096ULL) ||
      !paging_map_range(p->stack_phys, (uint64_t)PROCESS_STACK_PAGES * 4096ULL)) {
    kfree(p->kstack_alloc);
    free_pages(p->stack_phys, PROCESS_STACK_PAGES);
    free_pages(p->image_phys, p->image_pages);
    return PROCESS_ERR_NOMEM;
  }
  zero_pages(p->image_phys, p->image_pages);
  zero_pages(p->stack_phys, PROCESS_STACK_PAGES);

  if (!fat_read_file(vol, &ent, (void *)(uintptr_t)p->image_phys, (uint32_t)image_bytes, &got) ||
      got != image_bytes) {
    kfree(p->kstack_alloc);
    free_pages(p->stack_phys, PROCESS_STACK_PAGES);
    free_pages(p->image_phys, p->image_pages);
    return PROCESS_ERR_READ;
  }

  if (!paging_map_user(PROCESS_LOAD_BASE, p->image_phys, p->image_pages * 4096ULL, 1) ||
      !paging_map_user(stack_virt, p->stack_phys, (uint64_t)PROCESS_STACK_PAGES * 4096ULL, 1)) {
    paging_unmap_user(PROCESS_LOAD_BASE, p->image_pages * 4096ULL);
    paging_unmap_user(stack_virt, (uint64_t)PROCESS_STACK_PAGES * 4096ULL);
    kfree(p->kstack_alloc);
    free_pages(p->stack_phys, PROCESS_STACK_PAGES);
    free_pages(p->image_phys, p->image_pages);
    return PROCESS_ERR_NOMEM;
  }

  kstack = (uint8_t *)(((uintptr_t)p->kstack_alloc + 15U) & ~(uintptr_t)15U);
  p->kstack_top = (uint64_t)(uintptr_t)(kstack + PROCESS_KSTACK_SIZE);
  p->entry = PROCESS_LOAD_BASE;
  p->pid = g_next_pid++;
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
  code = user_run(p->entry, PROCESS_STACK_TOP - 8ULL, &p->saved_rsp);
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
