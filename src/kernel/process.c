#include "process.h"
#include "apps.h"
#include "fs.h"
#include "gdt.h"
#include "heap.h"
#include "paging.h"
#include "pmm.h"
#include "scheduler.h"
#include "string.h"
#include "syscall.h"
#include "usermode.h"
#include "irqlock.h"
#include "desktop.h"
#include "sysfile.h"
#include "syswin.h"

// The process running on the current task. Under the scheduler it hangs off the
// task's TCB (address_space field), so concurrent processes each find their own;
// g_fallback covers the unscheduled self-test.
static process_t *g_fallback = 0;
static uint64_t g_next_pid = 1;
static volatile int g_live = 0;
static volatile int g_live_max = 0;

static process_t *current_process(void) {
  TCB *self = scheduler_current_tcb();

  if (self != 0 && self->address_space != 0) {
    return (process_t *)self->address_space;
  }
  return g_fallback;
}

// The run state of the task executing now (see task_uctx_t); a process that is not run by a
// scheduler task keeps it in the process itself.
static task_uctx_t *cur_uctx(process_t *p) {
  TCB *self = scheduler_current_tcb();

  return (self != 0 && self->address_space == p) ? &self->uctx : &p->fb_uctx;
}

uint64_t process_current_pid(void) {
  process_t *p = current_process();

  return p != 0 ? p->pid : 0;
}

int process_pe_info(uint64_t out[4]) {
  process_t *p = current_process();

  if (p == 0 || !p->is_pe) {
    return 0;
  }
  out[0] = PE_HEAP_BASE + (uint64_t)PE_RESERVED_PAGES * 4096ULL;
  out[1] = (p->heap_pages - PE_RESERVED_PAGES) * 4096ULL;
  out[2] = p->pe_exe_base;
  out[3] = p->pe_dll_base;
  return 1;
}

const char *process_current_arg(void) {
  process_t *p = current_process();

  return (p != 0 && p->arg != 0) ? p->arg : "";
}

void **process_files_slot(void) {
  process_t *p = current_process();

  return p != 0 ? &p->files : 0;
}

int process_live_count(void) {
  return g_live;
}

int process_live_max(void) {
  return g_live_max;
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

static void create_cleanup(process_t *p) {
  if (p->cr3 != 0) {
    paging_destroy_address_space(p->cr3);
  }
  if (p->kstack_alloc != 0) {
    kfree(p->kstack_alloc);
  }
  if (p->stack_phys != 0) {
    free_pages(p->stack_phys, PROCESS_STACK_PAGES);
  }
  if (p->image_phys != 0) {
    free_pages(p->image_phys, p->image_pages);
  }
  if (p->heap_phys != 0) {
    free_pages(p->heap_phys, p->heap_pages);
  }
  p->heap_phys = 0;
  p->cr3 = 0;
  p->kstack_alloc = 0;
  p->stack_phys = 0;
  p->image_phys = 0;
}

// ---- Program images ----------------------------------------------------------
//
// A program file is either an ELF64 executable (ET_EXEC, x86-64, linked at a fixed
// address by src/user/user.ld) or, for older programs, a flat binary that is loaded
// whole at PROCESS_LOAD_BASE and starts at its first byte. ELF files are told apart by
// their magic number. Only PT_LOAD segments are used; there is no dynamic linking and
// no relocation, so the file must already be linked for the address it asks for.

#define ELF_MAX_SEGS 32
#define ELFCLASS64   2
#define ELFDATA2LSB  1
#define ET_EXEC      2
#define EM_X86_64    62
#define PT_LOAD      1
#define PF_X         1U
#define PF_W         2U

typedef struct {
  uint8_t  ident[16];
  uint16_t type, machine;
  uint32_t version;
  uint64_t entry, phoff, shoff;
  uint32_t flags;
  uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} elf64_ehdr_t;

typedef struct {
  uint32_t type, flags;
  uint64_t offset, vaddr, paddr, filesz, memsz, align;
} elf64_phdr_t;

typedef struct {
  uint64_t vaddr;       // where the segment starts in the process
  uint64_t filesz;      // bytes taken from the file
  uint64_t memsz;       // bytes in memory (the rest is zeroed .bss)
  uint64_t offset;      // position in the file
  int writable;
  int src;              // which file the bytes come from: 0 the program, 1 the Win32 DLL
} seg_t;

typedef struct {
  seg_t seg[ELF_MAX_SEGS];
  int count;
  uint64_t base;        // lowest page the image touches
  uint64_t end;         // one past the highest page, before the heap pages
  uint64_t entry;
} image_t;

static uint64_t page_down(uint64_t v) { return v & ~4095ULL; }
static uint64_t page_up(uint64_t v) { return (v + 4095ULL) & ~4095ULL; }

// Largest span an image may cover: from the load base up to the stack's guard page.
#define IMAGE_LIMIT (PROCESS_STACK_TOP - (uint64_t)PROCESS_STACK_PAGES * 4096ULL - 4096ULL)

// Describe a flat binary: one writable segment holding the whole file.
static void image_from_flat(uint64_t size, image_t *img) {
  img->count = 1;
  img->seg[0].vaddr = PROCESS_LOAD_BASE;
  img->seg[0].filesz = size;
  img->seg[0].memsz = size;
  img->seg[0].offset = 0;
  img->seg[0].writable = 1;
  img->seg[0].src = 0;
  img->base = PROCESS_LOAD_BASE;
  img->end = page_up(PROCESS_LOAD_BASE + size);
  img->entry = PROCESS_LOAD_BASE;
}

// Check an ELF header and its PT_LOAD segments and fill `img`. `buf` holds the first
// `have` bytes of the file, `size` is the whole file's length.
static int image_from_elf(const uint8_t *buf, uint64_t have, uint64_t size, image_t *img) {
  const elf64_ehdr_t *eh = (const elf64_ehdr_t *)buf;
  uint64_t lo = ~0ULL, hi = 0;
  int i, entry_ok = 0;

  if (have < sizeof(*eh) || eh->ident[4] != ELFCLASS64 || eh->ident[5] != ELFDATA2LSB ||
      eh->type != ET_EXEC || eh->machine != EM_X86_64 || eh->phentsize != sizeof(elf64_phdr_t) ||
      eh->phnum == 0 || eh->phoff > have || (uint64_t)eh->phnum * sizeof(elf64_phdr_t) > have - eh->phoff) {
    return 0;
  }
  img->count = 0;
  for (i = 0; i < eh->phnum; ++i) {
    elf64_phdr_t ph;
    seg_t *s;
    int j;

    memcpy(&ph, buf + eh->phoff + (uint64_t)i * sizeof(ph), sizeof(ph));
    if (ph.type != PT_LOAD || ph.memsz == 0) {
      continue;
    }
    if (img->count == ELF_MAX_SEGS || ph.filesz > ph.memsz || ph.offset > size ||
        ph.filesz > size - ph.offset || ph.vaddr < PROCESS_LOAD_BASE || ph.vaddr >= IMAGE_LIMIT ||
        ph.memsz > IMAGE_LIMIT - ph.vaddr) {
      return 0;
    }
    // The pages are what gets mapped, so two segments must not share one.
    for (j = 0; j < img->count; ++j) {
      uint64_t a0 = page_down(img->seg[j].vaddr), a1 = page_up(img->seg[j].vaddr + img->seg[j].memsz);
      if (page_down(ph.vaddr) < a1 && page_up(ph.vaddr + ph.memsz) > a0) {
        return 0;
      }
    }
    s = &img->seg[img->count++];
    s->vaddr = ph.vaddr;
    s->filesz = ph.filesz;
    s->memsz = ph.memsz;
    s->offset = ph.offset;
    s->writable = (ph.flags & PF_W) != 0;
    s->src = 0;
    if (page_down(ph.vaddr) < lo) {
      lo = page_down(ph.vaddr);
    }
    if (page_up(ph.vaddr + ph.memsz) > hi) {
      hi = page_up(ph.vaddr + ph.memsz);
    }
    if (eh->entry >= ph.vaddr && eh->entry < ph.vaddr + ph.memsz && (ph.flags & PF_X) != 0) {
      entry_ok = 1;
    }
  }
  if (img->count == 0 || !entry_ok) {
    return 0;
  }
  img->base = lo;
  img->end = hi;
  img->entry = eh->entry;
  return 1;
}

// ---- Windows (PE) programs -------------------------------------------------------
//
// A PE32+ (x64) executable is mapped like an ELF one, section by section, but at a load
// address of the kernel's choosing: the file's base relocations are applied. Next to it the
// Win32 runtime, /APPS/WIN32.DLL (built from src/win32), is mapped into the same address
// space and the executable's imports from kernel32, user32, gdi32 and friends are bound to
// that DLL's exports. Imports it does not export are bound to a stub that ends the program
// with a message naming nothing in particular (the kernel log says which ones were missing).
// The program gets a heap block (PE_HEAP_BASE) the DLL manages, found with SYS_PEINFO.
// Thread-local storage (the .tls section) works for the one thread: see pe_setup_teb.
// Not supported: SEH tables (an exception kills the process), delay-load, imports by
// ordinal, DLLs other than the runtime.

typedef struct {
  uint64_t image_base;        // ImageBase from the file
  uint64_t load_base;         // where it was put
  uint32_t size_of_image, entry_rva;
  uint32_t imp_rva, imp_size, rel_rva, rel_size, exp_rva, exp_size, tls_rva, tls_size;
  uint16_t chars, subsystem;
} pe_t;

static pe_t g_pe[2];          // 0 the program, 1 the Win32 DLL; only touched under process_create's preempt lock

static uint16_t u16le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t u32le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t u64le(const uint8_t *p) { return (uint64_t)u32le(p) | ((uint64_t)u32le(p + 4) << 32); }
static void put64le(uint8_t *p, uint64_t v) {
  int i;

  for (i = 0; i < 8; ++i) {
    p[i] = (uint8_t)(v >> (8 * i));
  }
}

static int image_add_seg(image_t *img, uint64_t vaddr, uint64_t filesz, uint64_t memsz, uint64_t offset, int writable,
                         int src) {
  seg_t *s;
  int j;

  if (img->count == ELF_MAX_SEGS || filesz > memsz || vaddr < PROCESS_LOAD_BASE || vaddr >= IMAGE_LIMIT ||
      memsz > IMAGE_LIMIT - vaddr) {
    return 0;
  }
  for (j = 0; j < img->count; ++j) {
    uint64_t a0 = page_down(img->seg[j].vaddr), a1 = page_up(img->seg[j].vaddr + img->seg[j].memsz);

    if (page_down(vaddr) < a1 && page_up(vaddr + memsz) > a0) {
      return 0;
    }
  }
  s = &img->seg[img->count++];
  s->vaddr = vaddr;
  s->filesz = filesz;
  s->memsz = memsz;
  s->offset = offset;
  s->writable = writable;
  s->src = src;
  if (page_down(vaddr) < img->base) {
    img->base = page_down(vaddr);
  }
  if (page_up(vaddr + memsz) > img->end) {
    img->end = page_up(vaddr + memsz);
  }
  return 1;
}

// Check the headers in b[0..have) and add the file's headers and sections to the image at
// `load_base`. `file_size` is the whole file's length.
#define PE_FAIL(code) do { console_printf("[PE] headers rejected (check %d)\r\n", (int)(code)); return 0; } while (0)

static int pe_load_headers(const uint8_t *b, uint64_t have, uint64_t file_size, uint64_t load_base, int src, pe_t *pe,
                           image_t *img) {
  uint32_t lfanew, nsec, size_opt, i, headers, sec_align, ndirs;
  const uint8_t *nt, *opt, *sec;

  memset(pe, 0, sizeof(*pe));
  if (have < 0x40 || b[0] != 'M' || b[1] != 'Z') {
    PE_FAIL(0);
  }
  lfanew = u32le(b + 0x3C);
  if (lfanew < 0x40 || lfanew > have || have - lfanew < 24) {
    PE_FAIL(1);
  }
  nt = b + lfanew;
  if (nt[0] != 'P' || nt[1] != 'E' || nt[2] != 0 || nt[3] != 0 || u16le(nt + 4) != 0x8664) {
    PE_FAIL(2);
  }
  nsec = u16le(nt + 6);
  size_opt = u16le(nt + 20);
  pe->chars = u16le(nt + 22);
  opt = nt + 24;
  if (size_opt < 112 || (uint64_t)(opt - b) + size_opt > have || u16le(opt) != 0x20B || nsec == 0 || nsec > 96) {
    PE_FAIL(3);
  }
  pe->entry_rva = u32le(opt + 16);
  pe->image_base = u64le(opt + 24);
  sec_align = u32le(opt + 32);
  pe->size_of_image = u32le(opt + 56);
  headers = u32le(opt + 60);
  pe->subsystem = u16le(opt + 68);
  ndirs = u32le(opt + 108);
  if (ndirs > (size_opt - 112u) / 8u) {
    ndirs = (size_opt - 112u) / 8u;
  }
  if (ndirs > 0) { pe->exp_rva = u32le(opt + 112); pe->exp_size = u32le(opt + 116); }
  if (ndirs > 1) { pe->imp_rva = u32le(opt + 120); pe->imp_size = u32le(opt + 124); }
  if (ndirs > 5) { pe->rel_rva = u32le(opt + 152); pe->rel_size = u32le(opt + 156); }
  if (ndirs > 9) { pe->tls_rva = u32le(opt + 184); pe->tls_size = u32le(opt + 188); }
  if (ndirs > 14 && u32le(opt + 228) != 0) {
    PE_FAIL(4);                                         // a .NET assembly
  }
  if (sec_align != 4096 || pe->size_of_image == 0 || (pe->size_of_image & 4095u) != 0 || headers == 0 ||
      headers > pe->size_of_image || headers > file_size || pe->entry_rva >= pe->size_of_image) {
    PE_FAIL(5);
  }
  sec = opt + size_opt;
  if ((uint64_t)(sec - b) + (uint64_t)nsec * 40u > have) {
    PE_FAIL(6);
  }
  pe->load_base = load_base;
  if (!image_add_seg(img, load_base, headers, page_up(headers), 0, 0, src)) {
    PE_FAIL(7);
  }
  for (i = 0; i < nsec; ++i) {
    const uint8_t *h = sec + (size_t)i * 40u;
    uint32_t vsize = u32le(h + 8), rva = u32le(h + 12), raw = u32le(h + 16), rawptr = u32le(h + 20);
    uint32_t flags = u32le(h + 36);
    uint64_t memsz = vsize != 0 ? vsize : raw, filesz = raw < memsz ? raw : memsz;

    if (memsz == 0) {
      continue;
    }
    if ((rva & 4095u) != 0 || (uint64_t)rva + memsz > pe->size_of_image || rva < headers ||
        (filesz != 0 && ((uint64_t)rawptr > file_size || filesz > file_size - rawptr))) {
      PE_FAIL(8);
    }
    if (!image_add_seg(img, load_base + rva, filesz, memsz, rawptr, (flags & 0x80000000u) != 0, src)) {
      PE_FAIL(9);
    }
  }
  return 1;
}

static int pe_ieq(const char *a, const char *b) {
  while (*a != 0 && char_lower(*a) == char_lower(*b)) {
    ++a;
    ++b;
  }
  return *a == 0 && *b == 0;
}

static int pe_streq(const char *a, const char *b) {
  while (*a != 0 && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

// A NUL-terminated string at `rva` inside the module, or 0 if it runs off the image.
static const char *pe_str(const uint8_t *m, const pe_t *pe, uint64_t rva) {
  uint64_t i;

  if (rva >= pe->size_of_image) {
    return 0;
  }
  for (i = rva; i < pe->size_of_image; ++i) {
    if (m[i] == 0) {
      return (const char *)(m + rva);
    }
  }
  return 0;
}

// RVA of the export called `name` (0 if there is none).
static uint32_t pe_export(const uint8_t *m, const pe_t *pe, const char *name) {
  const uint8_t *dir;
  uint32_t nnames, fns, names, ords, i;

  if (pe->exp_size < 40 || (uint64_t)pe->exp_rva + 40u > pe->size_of_image) {
    return 0;
  }
  dir = m + pe->exp_rva;
  nnames = u32le(dir + 24);
  fns = u32le(dir + 28);
  names = u32le(dir + 32);
  ords = u32le(dir + 36);
  for (i = 0; i < nnames; ++i) {
    const char *n;
    uint32_t ord, rva;

    if ((uint64_t)names + 4u * (uint64_t)(i + 1u) > pe->size_of_image || (uint64_t)ords + 2u * (uint64_t)(i + 1u) > pe->size_of_image) {
      return 0;
    }
    n = pe_str(m, pe, u32le(m + names + 4u * i));
    if (n == 0 || !pe_streq(n, name)) {
      continue;
    }
    ord = u16le(m + ords + 2u * i);
    if ((uint64_t)fns + 4u * (uint64_t)(ord + 1u) > pe->size_of_image) {
      return 0;
    }
    rva = u32le(m + fns + 4u * ord);
    if (rva >= pe->exp_rva && rva < pe->exp_rva + pe->exp_size) {
      return 0;                                       // a forwarder: not supported
    }
    return rva;
  }
  return 0;
}

// Apply the base relocations: the module sits at load_base, not at its ImageBase.
static int pe_relocate(uint8_t *m, const pe_t *pe) {
  uint64_t delta = pe->load_base - pe->image_base;
  uint32_t pos = 0;

  if (delta == 0) {
    return 1;
  }
  if (pe->rel_size == 0) {
    return (pe->chars & 1u) == 0;                     // no relocations needed unless they were stripped (IMAGE_FILE_RELOCS_STRIPPED)
  }
  if ((uint64_t)pe->rel_rva + pe->rel_size > pe->size_of_image) {
    return 0;
  }
  while (pos + 8u <= pe->rel_size) {
    const uint8_t *blk = m + pe->rel_rva + pos;
    uint32_t page = u32le(blk), size = u32le(blk + 4), n, k;

    if (size < 8 || pos + size > pe->rel_size) {
      return 0;
    }
    n = (size - 8u) / 2u;
    for (k = 0; k < n; ++k) {
      uint16_t w = u16le(blk + 8 + 2u * k);
      uint32_t type = w >> 12, rva = page + (w & 0xFFFu);

      if (type == 0) {
        continue;
      }
      if (type == 10 && (uint64_t)rva + 8u <= pe->size_of_image) {              // DIR64
        put64le(m + rva, u64le(m + rva) + delta);
      } else if (type == 3 && (uint64_t)rva + 4u <= pe->size_of_image) {        // HIGHLOW
        uint32_t v = u32le(m + rva) + (uint32_t)delta;

        m[rva] = (uint8_t)v;
        m[rva + 1] = (uint8_t)(v >> 8);
        m[rva + 2] = (uint8_t)(v >> 16);
        m[rva + 3] = (uint8_t)(v >> 24);
      } else {
        return 0;
      }
    }
    pos += size;
  }
  return 1;
}

static int pe_ieq_prefix(const char *s, const char *prefix) {
  while (*prefix != 0) {
    if (char_lower(*s) != char_lower(*prefix)) {
      return 0;
    }
    ++s;
    ++prefix;
  }
  return 1;
}

// DLL names the Win32 runtime stands in for.
static int pe_is_runtime_name(const char *n) {
  static const char names[][20] = {"kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "shell32.dll",
                                    "comctl32.dll", "win32.dll", "ucrtbase.dll", "vcruntime140.dll",
                                    "vcruntime140_1.dll", "msvcrt.dll"};
  size_t i;

  // The universal C runtime's API sets: api-ms-win-crt-stdio-l1-1-0.dll and so on.
  if (pe_ieq_prefix(n, "api-ms-win-crt-") || pe_ieq_prefix(n, "api-ms-win-core-")) {
    return 1;
  }
  for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    if (pe_ieq(names[i], n)) {
      return 1;
    }
  }
  return 0;
}

// Fill the program's import address table from the runtime's exports.
static int pe_bind_imports(uint8_t *m0, const uint8_t *m1) {
  const pe_t *pe = &g_pe[0], *dll = &g_pe[1];
  uint32_t pos, unimpl = pe_export(m1, dll, "__winclone_unimplemented");
  uint32_t missing = 0;

  if (pe->imp_size == 0) {
    return 1;
  }
  if ((uint64_t)pe->imp_rva + pe->imp_size > pe->size_of_image || unimpl == 0) {
    return 0;
  }
  for (pos = 0; pos + 20u <= pe->imp_size; pos += 20u) {
    const uint8_t *d = m0 + pe->imp_rva + pos;
    uint32_t ilt = u32le(d), name_rva = u32le(d + 12), iat = u32le(d + 16), k;
    const char *dname;
    int known;

    if (name_rva == 0 && iat == 0) {
      break;
    }
    dname = pe_str(m0, pe, name_rva);
    if (dname == 0 || iat == 0 || iat >= pe->size_of_image) {
      return 0;
    }
    known = pe_is_runtime_name(dname);
    for (k = 0;; ++k) {
      uint64_t thunk = (uint64_t)(ilt != 0 ? ilt : iat) + 8u * (uint64_t)k, slot = (uint64_t)iat + 8u * (uint64_t)k;
      uint64_t t, addr = 0;

      if (thunk + 8u > pe->size_of_image || slot + 8u > pe->size_of_image) {
        return 0;
      }
      t = u64le(m0 + thunk);
      if (t == 0) {
        break;
      }
      if ((t >> 63) == 0 && known) {
        const char *fname = pe_str(m0, pe, (t & 0x7FFFFFFFULL) + 2u);
        uint32_t rva = fname != 0 ? pe_export(m1, dll, fname) : 0;

        if (rva != 0) {
          addr = dll->load_base + rva;
        } else if (fname != 0) {
          console_printf("[PE] unresolved import %s!%s\r\n", dname, fname);
        }
      } else if ((t >> 63) == 0) {
        const char *fname = pe_str(m0, pe, (t & 0x7FFFFFFFULL) + 2u);

        console_printf("[PE] unresolved import %s!%s (no such library)\r\n", dname, fname != 0 ? fname : "?");
      } else {
        console_printf("[PE] unresolved import %s!#%llu (by ordinal)\r\n", dname, t & 0xFFFFULL);
      }
      if (addr == 0) {
        addr = dll->load_base + unimpl;
        ++missing;
      }
      put64le(m0 + slot, addr);
    }
  }
  if (missing != 0) {
    console_printf("[PE] %u import(s) not provided by the Win32 runtime: calling one ends the program\r\n", missing);
  }
  return 1;
}

// The thread block a Windows program expects at gs:[0]. It lives in the first page of the
// program's heap block, together with a PEB and the TLS pointer array; the TLS data (the exe's
// .tls section: initialised template, then zeroes) follows in the next pages. Returns 0 if the
// program's TLS does not fit.
static void pe_put(uint8_t *page, uint32_t off, uint64_t v) { put64le(page + off, v); }

static int pe_setup_teb(process_t *p, uint8_t *m0) {
  const pe_t *pe = &g_pe[0];
  uint8_t *teb = (uint8_t *)(uintptr_t)p->heap_phys;
  uint64_t base = PE_HEAP_BASE, stack_top = PROCESS_STACK_TOP;
  uint64_t peb = base + 0x400, tlsarr = base + 0x800, tlsdata = base + 4096ULL;

  pe_put(teb, 0x08, stack_top);                                    // NT_TIB.StackBase
  pe_put(teb, 0x10, stack_top - (uint64_t)PROCESS_STACK_PAGES * 4096ULL);   // StackLimit
  pe_put(teb, 0x30, base);                                         // Self
  pe_put(teb, 0x40, p->pid);                                       // ClientId.UniqueProcess
  pe_put(teb, 0x48, p->pid);                                       // ClientId.UniqueThread
  pe_put(teb, 0x58, tlsarr);                                       // ThreadLocalStoragePointer
  pe_put(teb, 0x60, peb);                                          // ProcessEnvironmentBlock
  pe_put(teb + 0x400, 0x10, pe->load_base);                        // PEB.ImageBaseAddress
  p->gs_base = base;
  if (pe->tls_size >= 40 && (uint64_t)pe->tls_rva + 40u <= pe->size_of_image) {
    const uint8_t *d = m0 + pe->tls_rva;
    uint64_t start = u64le(d), end = u64le(d + 8), idx = u64le(d + 16);
    uint32_t zero = u32le(d + 32);
    uint64_t lo = pe->load_base, hi = pe->load_base + pe->size_of_image, len, total;

    if (start < lo || end > hi || end < start || idx < lo || idx + 4u > hi) {
      return 0;
    }
    len = end - start;
    total = len + zero;
    if (total > (uint64_t)(PE_RESERVED_PAGES - 1U) * 4096ULL) {
      console_printf("[PE] thread-local storage of %llu bytes does not fit\r\n", total);
      return 0;
    }
    p->tls_off = start - lo;
    p->tls_len = len;
    p->tls_total = total;
    memcpy(teb + 4096, m0 + (start - lo), (size_t)len);
    pe_put(teb, 0x800, tlsdata);                                   // TLS slot 0
    memset(m0 + (idx - lo), 0, 4);                                 // _tls_index = 0
  }
  return 1;
}

// Both files are mapped (not yet in the address space): fix them up and set the start state.
static int pe_finish(process_t *p, image_t *img) {
  uint8_t *m0 = (uint8_t *)(uintptr_t)p->image_phys;
  uint8_t *m1 = m0 + (g_pe[1].load_base - img->base);
  uint32_t ret = pe_export(m1, &g_pe[1], "__winclone_return");
  uint64_t stack_virt = PROCESS_STACK_TOP - (uint64_t)PROCESS_STACK_PAGES * 4096ULL;

  if (!pe_relocate(m0, &g_pe[0])) {
    console_printf("[PE] the program cannot be relocated (no base relocations)\r\n");
    return PROCESS_ERR_BADPE;
  }
  if (!pe_relocate(m1, &g_pe[1])) {
    console_printf("[PE] the Win32 runtime cannot be relocated\r\n");
    return PROCESS_ERR_BADPE;
  }
  if (ret == 0) {
    console_printf("[PE] the Win32 runtime has no __winclone_return export\r\n");
    return PROCESS_ERR_BADPE;
  }
  if (!pe_bind_imports(m0, m1)) {
    console_printf("[PE] the program's import table is damaged\r\n");
    return PROCESS_ERR_BADPE;
  }
  if (!pe_setup_teb(p, m0)) {
    return PROCESS_ERR_BADPE;
  }
  img->entry = g_pe[0].load_base + g_pe[0].entry_rva;
  {
    // Entered through the runtime's start function, which runs the TLS callbacks and then the
    // program's own entry point (found with SYS_PEINFO).
    uint32_t start = pe_export(m1, &g_pe[1], "__winclone_start");
    uint32_t die = pe_export(m1, &g_pe[1], "__winclone_die");

    p->die_stub = die != 0 ? g_pe[1].load_base + die : 0;

    if (start != 0) {
      img->entry = g_pe[1].load_base + start;
    }
  }
  p->pe_exe_base = g_pe[0].load_base;
  p->pe_dll_base = g_pe[1].load_base;
  // Entered as a Windows function: RSP is 8 mod 16 with the return address on top and 32 bytes of
  // shadow space above it. Returning from the entry point ends the program with its result.
  p->initial_sp = PROCESS_STACK_TOP - 0x48ULL;
  put64le((uint8_t *)(uintptr_t)(p->stack_phys + (p->initial_sp - stack_virt)), g_pe[1].load_base + ret);
  return PROCESS_OK;
}

// Describe a PE program and the Win32 DLL it needs. Opens the DLL as *dll_file.
static int pe_setup(const uint8_t *head, uint64_t have, uint64_t size, image_t *img, fat_file_t *dll_file) {
  static uint8_t dhead[4096];
  fat_volume_t *vol = 0;
  fat_dirent_t ent;
  uint32_t got = 0;

  memset(img, 0, sizeof(*img));
  img->base = ~0ULL;
  if (!pe_load_headers(head, have, size, PROCESS_LOAD_BASE, 0, &g_pe[0], img) || (g_pe[0].chars & 0x2000) != 0 ||
      (g_pe[0].subsystem != 2 && g_pe[0].subsystem != 3)) {
    return PROCESS_ERR_BADPE;
  }
  if (!find_file(PE_SHIM_PATH, &vol, &ent)) {
    console_printf("[PE] the Win32 runtime %s was not found\r\n", PE_SHIM_PATH);
    return PROCESS_ERR_NOSHIM;
  }
  if (!fat_file_open(vol, &ent, dll_file) || !fat_file_read(dll_file, 0, dhead, sizeof(dhead), &got)) {
    return PROCESS_ERR_READ;
  }
  if (!pe_load_headers(dhead, got, ent.size, PROCESS_LOAD_BASE + g_pe[0].size_of_image, 1, &g_pe[1], img) ||
      (g_pe[1].chars & 0x2000) == 0 || g_pe[1].exp_size == 0) {
    return PROCESS_ERR_BADPE;
  }
  img->entry = PROCESS_LOAD_BASE;            // set for real by pe_finish
  return PROCESS_OK;
}

static int process_create_locked(const char *path, process_t *p) {
  fat_volume_t *vol = 0;
  fat_dirent_t ent;
  // Tasks have 4 KiB stacks, and process_create holds the preempt lock, so one copy of the big
  // locals will do (the image description alone is over a kilobyte).
  static fat_file_t file, dll_file;
  static image_t img;
  static uint8_t head[4096];
  uint32_t got = 0, head_len = 0;
  int is_pe = 0;
  uint64_t stack_virt = PROCESS_STACK_TOP - (uint64_t)PROCESS_STACK_PAGES * 4096ULL;
  uint64_t extra_virt;
  uint8_t *kstack;
  int i;

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
  if (!fat_file_open(vol, &ent, &file) ||
      !fat_file_read(&file, 0, head, sizeof(head), &head_len)) {
    return PROCESS_ERR_READ;
  }
  if (head_len >= 4 && head[0] == 0x7F && head[1] == 'E' && head[2] == 'L' && head[3] == 'F') {
    if (!image_from_elf(head, head_len, ent.size, &img)) {
      return PROCESS_ERR_BADELF;
    }
  } else if (head_len >= 2 && head[0] == 'M' && head[1] == 'Z') {
    int rc = pe_setup(head, head_len, ent.size, &img, &dll_file);

    if (rc != PROCESS_OK) {
      return rc;
    }
    is_pe = 1;
  } else {
    image_from_flat(ent.size, &img);
  }
  // The image, then a few zeroed pages after it for the heap.
  p->image_pages = (img.end - img.base) / 4096ULL + PROCESS_EXTRA_PAGES;
  if (img.base + p->image_pages * 4096ULL > IMAGE_LIMIT) {
    return PROCESS_ERR_TOOBIG;
  }

  p->image_phys = pmm_alloc_pages(p->image_pages);
  if (p->image_phys == 0) {
    return PROCESS_ERR_NOMEM;
  }
  p->stack_phys = pmm_alloc_pages(PROCESS_STACK_PAGES);
  p->kstack_alloc = kmalloc(PROCESS_KSTACK_SIZE + 16U);
  if (p->stack_phys == 0 || p->kstack_alloc == 0) {
    create_cleanup(p);
    return PROCESS_ERR_NOMEM;
  }

  // The kernel fills the pages through the identity map, so make sure it exists.
  if (!paging_map_range(p->image_phys, p->image_pages * 4096ULL) ||
      !paging_map_range(p->stack_phys, (uint64_t)PROCESS_STACK_PAGES * 4096ULL)) {
    create_cleanup(p);
    return PROCESS_ERR_NOMEM;
  }
  zero_pages(p->image_phys, p->image_pages);
  zero_pages(p->stack_phys, PROCESS_STACK_PAGES);
  if (is_pe) {
    p->heap_pages = PE_HEAP_PAGES;
    p->heap_phys = pmm_alloc_pages(p->heap_pages);
    if (p->heap_phys == 0 || !paging_map_range(p->heap_phys, p->heap_pages * 4096ULL)) {
      if (p->heap_phys != 0) {
        free_pages(p->heap_phys, p->heap_pages);
        p->heap_phys = 0;
      }
      create_cleanup(p);
      return PROCESS_ERR_NOMEM;
    }
    zero_pages(p->heap_phys, p->heap_pages);
    p->is_pe = 1;
  }

  // Copy each segment's file bytes to its place; the rest of it stays zero (.bss).
  for (i = 0; i < img.count; ++i) {
    const seg_t *s = &img.seg[i];
    uint8_t *dst = (uint8_t *)(uintptr_t)(p->image_phys + (s->vaddr - img.base));

    if (s->filesz != 0 && (!fat_file_read(s->src ? &dll_file : &file, (uint32_t)s->offset, dst, (uint32_t)s->filesz, &got) ||
                           got != s->filesz)) {
      create_cleanup(p);
      return PROCESS_ERR_READ;
    }
  }
  if (is_pe) {
    int rc = pe_finish(p, &img);

    if (rc != PROCESS_OK) {
      create_cleanup(p);
      return rc;
    }
  }

  // A private address space: it shares the kernel's mappings and maps the image
  // and stack at the same virtual addresses every process uses. Segments the file
  // does not mark writable (code, constants) are mapped read-only.
  p->cr3 = paging_create_address_space();
  if (p->cr3 == 0) {
    create_cleanup(p);
    return PROCESS_ERR_NOMEM;
  }
  for (i = 0; i < img.count; ++i) {
    const seg_t *s = &img.seg[i];
    uint64_t v0 = page_down(s->vaddr), v1 = page_up(s->vaddr + s->memsz);

    if (!paging_map_user_in(p->cr3, v0, p->image_phys + (v0 - img.base), v1 - v0, s->writable)) {
      create_cleanup(p);
      return PROCESS_ERR_NOMEM;
    }
  }
  // Heap pages, then the stack.
  extra_virt = img.end;
  if (!paging_map_user_in(p->cr3, extra_virt, p->image_phys + (extra_virt - img.base),
                          (uint64_t)PROCESS_EXTRA_PAGES * 4096ULL, 1) ||
      !paging_map_user_in(p->cr3, stack_virt, p->stack_phys, (uint64_t)PROCESS_STACK_PAGES * 4096ULL, 1) ||
      (is_pe && !paging_map_user_in(p->cr3, PE_HEAP_BASE, p->heap_phys, p->heap_pages * 4096ULL, 1))) {
    create_cleanup(p);
    return PROCESS_ERR_NOMEM;
  }

  kstack = (uint8_t *)(((uintptr_t)p->kstack_alloc + 15U) & ~(uintptr_t)15U);
  p->kstack_top = (uint64_t)(uintptr_t)(kstack + PROCESS_KSTACK_SIZE);
  p->entry = img.entry;
  {
    // Under the scheduler the pid is the id of the task running the program.
    TCB *self = scheduler_current_tcb();
    p->pid = (self != 0) ? (uint64_t)self->task_id : g_next_pid++;
  }
  p->loaded = 1;
  {
    uint64_t flags = irq_save();
    g_live++;
    if (g_live > g_live_max) {
      g_live_max = g_live;
    }
    irq_restore(flags);
  }
  return PROCESS_OK;
}

// Finding the file and reading it are separate FAT calls; keep other tasks from
// switching in between so the directory cannot change under the loader.
int process_create(const char *path, process_t *p) {
  int rc;

  scheduler_preempt_disable();
  rc = process_create_locked(path, p);
  scheduler_preempt_enable();
  return rc;
}

// ---- Threads --------------------------------------------------------------------------------

#define THREAD_VBASE        0x00007F0001000000ULL   // slot i: a guard page, the stack, then the TEB pages
#define THREAD_STRIDE       0x20000ULL
#define THREAD_MAX          15
#define THREAD_STACK_PAGES  16U
#define THREAD_PAGES        (THREAD_STACK_PAGES + PE_RESERVED_PAGES)

typedef struct {
  int used;
  uint64_t tid;
  void *proc;                 // the process_t
  uint64_t phys;              // the stack pages, then the TEB pages
  uint64_t stack_virt, stack_top;   // stack_top is also the TEB's address
  uint64_t stub;              // user code the thread starts at
  void *kstack_alloc;
  uint64_t kstack_top;
  TCB *tcb;                   // the task running the thread, once it has started
} thread_t;

static uint64_t g_next_tid = 0x1000;

static void thread_wait_tick(void) {
  __asm__ volatile("sti; hlt" : : : "memory");
}

// Ask `t` to end. A task blocked in a syscall is woken (its wait sees the flag); one that was
// preempted in ring 3 is redirected to WIN32.DLL's __winclone_die, which makes the exit syscall.
static void request_kill(process_t *p, TCB *t) {
  uint64_t flags = irq_save();

  t->uctx.kill = 1;
  if (t->state == TASK_STATE_BLOCKED) {
    scheduler_task_unblock(t);
  } else if (t->state == TASK_STATE_READY && t->stack_pointer != 0 && p->die_stub != 0) {
    uint64_t *frame = (uint64_t *)t->stack_pointer;

    if ((frame[16] & 3ULL) == 3ULL) {             // saved CS: ring 3 (rax ... r15, rip, cs, rflags, rsp, ss)
      frame[0] = SYS_EXIT;                        // rax
      frame[5] = p->exit_code;                    // rdi
      frame[15] = p->die_stub;                    // rip
    }
  }
  irq_restore(flags);
}

// Start ending the process from `except`'s point of view: every other thread, the main one
// included, is asked to end.
static void kill_others(process_t *p, TCB *except) {
  thread_t *tab = (thread_t *)p->threads;
  int i;

  p->exiting = 1;
  if (p->main_tcb != 0 && (TCB *)p->main_tcb != except) {
    request_kill(p, (TCB *)p->main_tcb);
  }
  for (i = 0; tab != 0 && i < THREAD_MAX; ++i) {
    if (tab[i].used && tab[i].tcb != 0 && tab[i].tcb != except) {
      request_kill(p, tab[i].tcb);
    }
  }
}

// The main thread is done: end the others and wait until their tasks have gone.
static void threads_stop(process_t *p) {
  if (p->threads == 0) {
    return;
  }
  p->exiting = 1;
  kill_others(p, scheduler_current_tcb());
  while (p->live_threads > 0) {
    thread_wait_tick();
  }
  kfree(p->threads);
  p->threads = 0;
}

static void thread_put(uint8_t *page, uint32_t off, uint64_t v) {
  put64le(page + off, v);
}

static int thread_task(void *arg) {
  thread_t *t = (thread_t *)arg;
  process_t *p = (process_t *)t->proc;
  TCB *self = scheduler_current_tcb();
  uint64_t flags, cr3 = p->cr3;

  flags = irq_save();
  memset(&self->uctx, 0, sizeof(self->uctx));
  self->address_space = p;
  self->cr3 = p->cr3;
  self->kernel_rsp0 = t->kstack_top;
  self->gs_base = t->stack_top;
  self->thread = t;
  t->tcb = self;
  paging_activate(p->cr3);
  gdt_set_kernel_stack(t->kstack_top);
  cpu_set_gs_base(t->stack_top);
  irq_restore(flags);

  if (!p->exiting) {
    self->uctx.running = 1;
    user_run(t->stub, t->stack_top - 0x28ULL, &self->uctx.saved_rsp);
    self->uctx.running = 0;
  }

  // Release what belongs to this thread; the process stays.
  paging_unmap_user_in(cr3, t->stack_virt, (uint64_t)THREAD_PAGES * 4096ULL);
  free_pages(t->phys, THREAD_PAGES);
  kfree(t->kstack_alloc);
  flags = irq_save();
  self->address_space = 0;
  self->cr3 = 0;
  self->gs_base = 0;
  self->kernel_rsp0 = 0;
  self->thread = 0;
  paging_activate(0);
  cpu_set_gs_base(0);
  t->used = 0;
  t->tcb = 0;
  p->live_threads--;                  // the last use of p: process_destroy may go on from here
  irq_restore(flags);
  return 0;
}

int64_t process_thread_create(uint64_t stub, uint64_t fn, uint64_t arg) {
  process_t *p = current_process();
  thread_t *tab, *t;
  uint64_t base, flags;
  uint8_t *teb, *kstack;
  int i;

  if (p == 0 || !p->is_pe || scheduler_current_tcb() == 0) {
    return SYSCALL_ENOSYS;
  }
  if (p->exiting) {
    return SYSCALL_EBUSY;
  }
  if (!paging_user_range_ok(stub, 1) || !paging_user_range_ok(fn, 1)) {
    return SYSCALL_EFAULT;
  }
  if (p->threads == 0) {
    p->threads = kmalloc(sizeof(thread_t) * THREAD_MAX);
    if (p->threads == 0) {
      return SYSCALL_ENOMEM;
    }
    memset(p->threads, 0, sizeof(thread_t) * THREAD_MAX);
  }
  tab = (thread_t *)p->threads;
  for (i = 0; i < THREAD_MAX && tab[i].used; ++i) {
  }
  if (i == THREAD_MAX) {
    return SYSCALL_ENOMEM;
  }
  t = &tab[i];
  memset(t, 0, sizeof(*t));
  t->used = 1;
  t->proc = p;
  t->stub = stub;
  t->tid = g_next_tid++;
  base = THREAD_VBASE + (uint64_t)i * THREAD_STRIDE;
  t->stack_virt = base + 4096ULL;
  t->stack_top = t->stack_virt + (uint64_t)THREAD_STACK_PAGES * 4096ULL;
  t->phys = pmm_alloc_pages(THREAD_PAGES);
  t->kstack_alloc = kmalloc(PROCESS_KSTACK_SIZE + 16U);
  if (t->phys == 0 || t->kstack_alloc == 0 || !paging_map_range(t->phys, (uint64_t)THREAD_PAGES * 4096ULL) ||
      !paging_map_user_in(p->cr3, t->stack_virt, t->phys, (uint64_t)THREAD_PAGES * 4096ULL, 1)) {
    if (t->phys != 0) {
      free_pages(t->phys, THREAD_PAGES);
    }
    if (t->kstack_alloc != 0) {
      kfree(t->kstack_alloc);
    }
    t->used = 0;
    return SYSCALL_ENOMEM;
  }
  zero_pages(t->phys, THREAD_PAGES);
  kstack = (uint8_t *)(((uintptr_t)t->kstack_alloc + 15U) & ~(uintptr_t)15U);
  t->kstack_top = (uint64_t)(uintptr_t)(kstack + PROCESS_KSTACK_SIZE);

  // The thread block: like the main thread's (pe_setup_teb), with its own TLS copy.
  teb = (uint8_t *)(uintptr_t)(t->phys + (uint64_t)THREAD_STACK_PAGES * 4096ULL);
  thread_put(teb, 0x08, t->stack_top);
  thread_put(teb, 0x10, t->stack_virt);
  thread_put(teb, 0x30, t->stack_top);
  thread_put(teb, 0x40, p->pid);
  thread_put(teb, 0x48, t->tid);
  thread_put(teb, 0x58, t->stack_top + 0x800ULL);
  thread_put(teb, 0x60, PE_HEAP_BASE + 0x400ULL);
  thread_put(teb, 0x300, fn);
  thread_put(teb, 0x308, arg);
  if (p->tls_total != 0) {
    memcpy(teb + 4096, (const uint8_t *)(uintptr_t)(p->image_phys + p->tls_off), (size_t)p->tls_len);
    thread_put(teb, 0x800, t->stack_top + 4096ULL);
  }

  flags = irq_save();
  p->live_threads++;
  irq_restore(flags);
  if (!scheduler_add_task("thread", thread_task, t)) {
    flags = irq_save();
    p->live_threads--;
    irq_restore(flags);
    paging_unmap_user_in(p->cr3, t->stack_virt, (uint64_t)THREAD_PAGES * 4096ULL);
    free_pages(t->phys, THREAD_PAGES);
    kfree(t->kstack_alloc);
    t->used = 0;
    return SYSCALL_ENOMEM;
  }
  return (int64_t)t->tid;
}

int process_kill_pending(void) {
  process_t *p = current_process();

  return p != 0 && cur_uctx(p)->kill;
}

// Called by the exit syscall on the process's kernel stack: jump back into process_run's
// caller (or, for an extra thread, into its task function). An extra thread calling exit()
// ends the whole program. Never returns.
static void process_exit_handler(uint64_t code) {
  process_t *p = current_process();
  TCB *self = scheduler_current_tcb();

  if (!p->exiting) {
    p->exit_code = code;
    kill_others(p, self);
  }
  user_return(cur_uctx(p)->saved_rsp, p->exit_code);
}

void process_thread_exit(uint64_t code) {
  process_t *p = current_process();
  TCB *self = scheduler_current_tcb();

  if (p == 0) {
    return;
  }
  if (self != 0 && self->thread != 0) {
    user_return(self->uctx.saved_rsp, code);
  }
  process_exit_handler(code);                       // the main thread: the program ends
}

uint64_t process_run(process_t *p) {
  uint64_t code;
  uint64_t flags;
  TCB *self;

  if (p == 0 || !p->loaded) {
    return (uint64_t)-1;
  }
  syscall_set_current_pid(p->pid);
  syscall_set_exit_handler(process_exit_handler);

  // Make this task a process: from now on the scheduler loads its address space
  // and kernel stack whenever the task is switched in. Do the same right now for
  // the running CPU. Interrupts are off so no switch can fall in between.
  flags = irq_save();
  self = scheduler_current_tcb();
  if (self != 0) {
    memset(&self->uctx, 0, sizeof(self->uctx));
    self->thread = 0;
    self->address_space = p;
    self->cr3 = p->cr3;
    self->kernel_rsp0 = p->kstack_top;
    self->gs_base = p->gs_base;
    p->main_tcb = self;
  } else {
    memset(&p->fb_uctx, 0, sizeof(p->fb_uctx));
    g_fallback = p;
  }
  paging_activate(p->cr3);
  gdt_set_kernel_stack(p->kstack_top);
  cpu_set_gs_base(p->gs_base);
  irq_restore(flags);

  // The program starts as if called: RSP is 8 mod 16 at function entry.
  if (p->is_pe && p->heap_phys != 0) {
    put64le((uint8_t *)(uintptr_t)p->heap_phys + 0x40, p->pid);                 // TEB ClientId
    put64le((uint8_t *)(uintptr_t)p->heap_phys + 0x48, p->pid);
  }
  cur_uctx(p)->running = 1;
  code = user_run(p->entry, p->initial_sp != 0 ? p->initial_sp : PROCESS_STACK_TOP - 8ULL, &cur_uctx(p)->saved_rsp);
  cur_uctx(p)->running = 0;
  p->exit_code = code;
  return code;
}

void process_destroy(process_t *p) {
  uint64_t flags;
  TCB *self;

  if (p == 0 || !p->loaded) {
    return;
  }
  threads_stop(p);
  // Leave the address space before freeing it, and make sure the scheduler does
  // not switch back to it.
  flags = irq_save();
  self = scheduler_current_tcb();
  if (self != 0 && self->address_space == p) {
    self->address_space = 0;
    self->cr3 = 0;
    self->gs_base = 0;
  }
  if (g_fallback == p) {
    g_fallback = 0;
  }
  paging_activate(0);
  irq_restore(flags);

  // Files the program left open: store pending writes unless it was killed by a fault.
  sysfile_close_all(p->files, p->exit_code < PROCESS_EXIT_FAULT_BASE);
  p->files = 0;
  syswin_process_exit(p->pid);
  create_cleanup(p);
  p->loaded = 0;
  flags = irq_save();
  g_live--;
  irq_restore(flags);
}

int process_handle_user_fault(uint64_t vector, uint64_t error_code, uint64_t rip, uint64_t fault_addr) {
  process_t *p = current_process();

  if (p == 0 || !cur_uctx(p)->running) {
    return 0;
  }
  console_printf("[PROC] pid %llu killed: exception %llu (error 0x%llx) at rip 0x%llx, address 0x%llx\r\n",
                 p->pid, vector, error_code, rip, fault_addr);
  if (!p->exiting) {
    p->exit_code = PROCESS_EXIT_FAULT_BASE + vector;
    kill_others(p, scheduler_current_tcb());
  }
  // We are on the process's kernel stack; unwind to the code that called process_run.
  user_return(cur_uctx(p)->saved_rsp, p->exit_code);
}

void process_syscall_enter(void) {
  process_t *p = current_process();

  if (p != 0) {
    task_uctx_t *u = cur_uctx(p);

    u->preempt_level = scheduler_preempt_level();
    u->in_syscall = 1;
  }
}

void process_syscall_leave(void) {
  process_t *p = current_process();

  if (p != 0) {
    task_uctx_t *u = cur_uctx(p);

    u->in_syscall = 0;
    if (u->kill) {                                  // another thread ended the process: leave now
      scheduler_preempt_restore(u->preempt_level);
      user_return(u->saved_rsp, p->exit_code);
    }
  }
}

int process_handle_kernel_fault(uint64_t vector, uint64_t error_code, uint64_t rip, uint64_t fault_addr) {
  process_t *p = current_process();

  // NMI, double fault and machine check say the kernel itself is broken; do not
  // try to carry on from those.
  if (vector == 2 || vector == 8 || vector == 18) {
    return 0;
  }
  if (p == 0 || !cur_uctx(p)->running || !cur_uctx(p)->in_syscall) {
    return 0;
  }
  console_printf("[PROC] pid %llu killed: kernel fault in syscall: exception %llu (error 0x%llx) at rip 0x%llx, address 0x%llx\r\n",
                 p->pid, vector, error_code, rip, fault_addr);
  // The aborted syscall may have been holding the FAT driver's preempt lock.
  scheduler_preempt_restore(cur_uctx(p)->preempt_level);
  cur_uctx(p)->in_syscall = 0;
  if (!p->exiting) {
    p->exit_code = PROCESS_EXIT_FAULT_BASE + vector;
    kill_others(p, scheduler_current_tcb());
  }
  // We are on the process's kernel stack; unwind to the code that called process_run.
  user_return(cur_uctx(p)->saved_rsp, p->exit_code);
}

// ---- Spawning programs as scheduler tasks -----------------------------------

#define SPAWN_MAX 8

#define OUT_RING 4096U
#define IN_RING 256U

// Per-program console: what the program wrote, and what was typed for it.
typedef struct {
  char out[OUT_RING];
  volatile uint64_t out_total;
  char in[IN_RING];
  volatile uint32_t in_head;      // next byte to read
  volatile uint32_t in_tail;      // next free slot
  volatile int in_eof;
} proc_io_t;

typedef struct {
  int used;
  volatile int done;
  volatile uint64_t pid;           // process id once loaded, 0 before and after the run
  proc_io_t io;
  int error;
  int detached;                   // nobody waits: release the slot when the program ends
  uint64_t exit_code;
  char path[64];
  char arg[PROCESS_ARG_MAX];
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
    if (s->detached) {
      console_printf("[PROC] %s did not load (error %d)\r\n", s->path, rc);
      s->used = 0;
    }
    return 0;
  }

  p.io = &s->io;
  p.arg = s->arg;
  s->pid = p.pid;
  s->exit_code = process_run(&p);
  s->pid = 0;
  {
    TCB *self = scheduler_current_tcb();
    console_printf("[PROC] pid %llu (%s) exited with %llu after %u context switches\r\n",
                   p.pid, s->path, s->exit_code, self != 0 ? self->context_switches : 0U);
  }
  process_destroy(&p);
  s->error = PROCESS_OK;
  s->done = 1;
  if (s->detached) {
    s->used = 0;
  }
  return 0;
}

int process_spawn(const char *path) {
  return process_spawn_arg(path, 0);
}

int process_program_exists(const char *path) {
  fat_volume_t *vol = 0;
  fat_dirent_t ent;

  return find_file(path, &vol, &ent) && ent.size != 0;
}

int process_spawn_active(const char *path, uint64_t *pid) {
  int i;

  if (pid != 0) {
    *pid = 0;
  }
  for (i = 0; i < SPAWN_MAX; ++i) {
    spawn_t *s = &g_spawns[i];

    if (s->used && !s->done && str_eq_nocase(s->path, path)) {
      if (pid != 0) {
        *pid = s->pid;
      }
      return 1;
    }
  }
  return 0;
}

static int spawn_common(const char *path, const char *arg, int detached);

int process_spawn_arg(const char *path, const char *arg) {
  return spawn_common(path, arg, 0);
}

int process_spawn_detached(const char *path, const char *arg) {
  int h = spawn_common(path, arg, 1);

  return h < 0 ? h : 0;
}

static int spawn_common(const char *path, const char *arg, int detached) {
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
    str_copy(s->arg, arg != 0 ? arg : "", sizeof(s->arg));
    s->done = 0;
    s->detached = detached;
    s->io.out_total = 0;
    s->io.in_head = 0;
    s->io.in_tail = 0;
    s->io.in_eof = 0;
    s->error = PROCESS_OK;
    s->exit_code = 0;
    if (!scheduler_add_task(s->name, spawn_task, s)) {
      s->used = 0;
      return PROCESS_ERR_NOMEM;
    }
  }
  return handle;
}

// ---- Program output and input ------------------------------------------------
// Each spawned program has its own output ring and input queue (proc_io_t), so
// several programs can run side by side, each with its own GUI window. Readers keep
// their own cursor (total bytes seen); torn reads are harmless. A program that was
// not spawned (self-tests) has no console: its output is dropped, reads see EOF.

static proc_io_t *current_io(void) {
  process_t *p = current_process();

  return p != 0 ? (proc_io_t *)p->io : 0;
}

static proc_io_t *handle_io(int handle) {
  if (handle < 0 || handle >= SPAWN_MAX || !g_spawns[handle].used) {
    return 0;
  }
  return &g_spawns[handle].io;
}

void process_output_write(const char *data, uint64_t len) {
  proc_io_t *io = current_io();
  uint64_t i;

  if (io == 0) {
    return;
  }
  for (i = 0; i < len; ++i) {
    io->out[io->out_total % OUT_RING] = data[i];
    io->out_total = io->out_total + 1;
  }
}

uint64_t process_output_read(int handle, uint64_t *cursor, char *buf, uint64_t max) {
  proc_io_t *io = handle_io(handle);
  uint64_t total, n = 0;

  if (io == 0) {
    return 0;
  }
  total = io->out_total;
  if (total - *cursor > OUT_RING) {
    *cursor = total - OUT_RING;       // fell behind: skip what was overwritten
  }
  while (*cursor < total && n < max) {
    buf[n++] = io->out[*cursor % OUT_RING];
    *cursor = *cursor + 1;
  }
  return n;
}

void process_input_push(int handle, char c) {
  proc_io_t *io = handle_io(handle);
  uint64_t f;

  if (io == 0) {
    return;
  }
  f = irq_save();
  if (io->in_tail - io->in_head < IN_RING) {
    io->in[io->in_tail % IN_RING] = c;
    io->in_tail = io->in_tail + 1;
  }
  irq_restore(f);
  scheduler_wake_all(TASK_WAIT_INPUT);
}

void process_input_close(int handle) {
  proc_io_t *io = handle_io(handle);

  if (io == 0) {
    return;
  }
  io->in_eof = 1;
  scheduler_wake_all(TASK_WAIT_INPUT);
}

uint64_t process_input_read(char *buf, uint64_t max) {
  proc_io_t *io = current_io();
  uint64_t n = 0;
  uint64_t f;

  if (io == 0) {
    return 0;
  }
  f = irq_save();
  while (io->in_head != io->in_tail && n < max) {
    buf[n++] = io->in[io->in_head % IN_RING];
    io->in_head = io->in_head + 1;
  }
  irq_restore(f);
  return n;
}

int process_input_eof(void) {
  proc_io_t *io = current_io();

  return io == 0 || io->in_eof;
}

int process_finished(int handle) {
  return handle >= 0 && handle < SPAWN_MAX && g_spawns[handle].used && g_spawns[handle].done;
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

#if defined(KERNEL_USER_SELFTEST) && KERNEL_USER_SELFTEST == 6
// USER_SELFTEST=6: several processes loaded at once, each in its own address
// space at the same virtual addresses, must not see each other's memory.
static int iso_expect(int ok, const char *what) {
  console_printf("[ISO]  %s %s\r\n", ok ? "ok  " : "FAIL", what);
  return ok ? 0 : 1;
}

#define ISO_PROCS 4

static int isolation_supervisor_task(void *arg) {
  int handles[ISO_PROCS];
  int failures = 0;
  uint64_t code;
  int i;
  int started = 0;

  (void)arg;
  console_write_string("[ISO] spawning concurrent processes\r\n");
  for (i = 0; i < ISO_PROCS; ++i) {
    handles[i] = process_spawn("/APPS/ISO.BIN");
    if (handles[i] >= 0) {
      ++started;
    }
  }
  failures += iso_expect(started == ISO_PROCS, "all spawned");
  for (i = 0; i < ISO_PROCS; ++i) {
    int ok = handles[i] >= 0 && process_wait(handles[i], &code) == PROCESS_OK && code == 0;
    failures += iso_expect(ok, "process kept its own memory (exit 0)");
  }
  console_printf("[ISO] most processes loaded at once: %d\r\n", process_live_max());
  failures += iso_expect(process_live_max() >= 3, "processes really ran concurrently");
  failures += iso_expect(process_live_count() == 0, "all address spaces released");

  // A crash must still be contained while others are alive, and loading again
  // after everything was freed must work.
  {
    int h_crash = process_spawn("/APPS/CRASH.BIN");
    int h_iso = process_spawn("/APPS/ISO.BIN");
    failures += iso_expect(h_crash >= 0 && process_wait(h_crash, &code) == PROCESS_OK &&
                               code == PROCESS_EXIT_FAULT_BASE + 14,
                           "crash contained next to a live process");
    failures += iso_expect(h_iso >= 0 && process_wait(h_iso, &code) == PROCESS_OK && code == 0,
                           "neighbour of the crash was unaffected");
  }
  for (i = 0; i < 12; ++i) {
    int h = process_spawn("/APPS/HELLO.BIN");
    if (h < 0 || process_wait(h, &code) != PROCESS_OK || code != 7) {
      break;
    }
  }
  failures += iso_expect(i == 12, "twelve more launches (tables and pages recycled)");
  failures += iso_expect(process_live_count() == 0, "nothing leaked at the end");

  if (failures == 0) {
    console_write_string("[ISO] PASS: processes have separate address spaces\r\n");
  } else {
    console_printf("[ISO] FAIL: %d check(s) failed\r\n", failures);
  }
  return 0;
}

void process_isolation_selftest(void) {
  fs_init();
  scheduler_add_task("iso-supervisor", isolation_supervisor_task, 0);
}
#endif

#if defined(KERNEL_USER_SELFTEST) && KERNEL_USER_SELFTEST == 7
// USER_SELFTEST=7: the file syscalls. FILES.BIN checks open/read/write/close/readdir
// and their errors itself and leaves a file open when it exits; running it twice
// proves the kernel stored that file at exit (the second run finds it).
static int file_expect(int ok, const char *what) {
  console_printf("[FILE]  %s %s\r\n", ok ? "ok  " : "FAIL", what);
  return ok ? 0 : 1;
}

static int file_supervisor_task(void *arg) {
  int failures = 0;
  uint64_t code = 0;
  int h;
  int i;

  (void)arg;
  h = process_spawn("/APPS/FILES.BIN");
  failures += file_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && (code == 0 || code == 50),
                          "first run: every check passed");
  console_printf("[FILE] first run exit code %llu\r\n", code);
  h = process_spawn_arg("/APPS/FILES.BIN", "spawn");      // this run also spawns a program
  failures += file_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == 0,
                          "second run: file left open was stored at exit");
  {
    // Two at once, each with its own descriptor table.
    int a = process_spawn("/APPS/FILES.BIN");
    int b = process_spawn("/APPS/FILES.BIN");
    uint64_t ca = 1, cb = 1;
    failures += file_expect(a >= 0 && b >= 0 && process_wait(a, &ca) == PROCESS_OK &&
                                process_wait(b, &cb) == PROCESS_OK && ca == 0 && cb == 0,
                            "two concurrent runs");
  }
  for (i = 0; i < 6; ++i) {
    h = process_spawn("/APPS/FILES.BIN");
    if (h < 0 || process_wait(h, &code) != PROCESS_OK || code != 0) {
      break;
    }
  }
  failures += file_expect(i == 6, "six more runs (descriptor tables and buffers freed)");
  for (i = 0; i < 500 && process_live_count() != 0; ++i) {
    wait_one_tick();                                          // the detached HELLO.BIN finishes
  }
  failures += file_expect(process_live_count() == 0, "nothing leaked at the end (detached spawn finished)");

  if (failures == 0) {
    console_write_string("[FILE] PASS: file syscalls\r\n");
  } else {
    console_printf("[FILE] FAIL: %d check(s) failed\r\n", failures);
  }
  return 0;
}

void process_file_selftest(void) {
  fs_init();
  scheduler_add_task("file-supervisor", file_supervisor_task, 0);
}
#endif

#if defined(KERNEL_USER_SELFTEST) && KERNEL_USER_SELFTEST == 8
// USER_SELFTEST=8: window syscalls, with the desktop running as a task. WINTEST.BIN
// checks the calls and leaves windows open; the kernel must release them at exit.
// WINEV.BIN waits for input events that this task injects.
static int win_expect(int ok, const char *what) {
  console_printf("[WIN]  %s %s\r\n", ok ? "ok  " : "FAIL", what);
  return ok ? 0 : 1;
}

static int wait_for(int (*cond)(void), int ticks) {
  while (ticks-- > 0 && !cond()) {
    wait_one_tick();
  }
  return cond();
}

static int cond_has_window(void) { return syswin_test_active() > 0; }
static int cond_no_windows(void) { return syswin_test_used() == 0; }
static int cond_paint_white(void) { return syswin_test_count(0x00FFFFFFu) >= 100000; }
static int cond_paint_red(void) { return syswin_test_pixel(105, 100) == 0x00C82828; }
static int cond_paint_cleared(void) { return syswin_test_pixel(105, 100) == 0x00FFFFFF; }
static int cond_about_title(void) { return syswin_test_count(0x002A5ABEu) >= 100; }
static int cond_size_300(void) {
  int w = 0, h = 0;

  return syswin_test_size(&w, &h) && w == 300 && h == 200;
}
static int cond_size_800(void) {
  int w = 0, h = 0;

  return syswin_test_size(&w, &h) && w == 800 && h == 600;
}

static int name_before(const char *a, const char *b) {   // case-insensitive a < b
  while (*a != 0 && *b != 0 && char_upper(*a) == char_upper(*b)) {
    ++a;
    ++b;
  }
  return char_upper(*a) < char_upper(*b);
}

static int cond_explorer_folder(void) {
  fat_volume_t *v = fs_volume(0);
  fat_dirent_t e;

  return v != 0 && fat_lookup(v, v->root_cluster, "New Folder", &e);
}
static int cond_explorer_folder_gone(void) { return !cond_explorer_folder(); }
static int cond_explorer_two_folders(void) {
  fat_volume_t *v = fs_volume(0);
  fat_dirent_t e;

  return v != 0 && fat_lookup(v, v->root_cluster, "New Folder", &e) &&
         fat_lookup(v, v->root_cluster, "New Folder 2", &e);
}
static int cond_explorer_two_gone(void) {
  fat_volume_t *v = fs_volume(0);
  fat_dirent_t e;

  return v != 0 && !fat_lookup(v, v->root_cluster, "New Folder 2", &e);
}

static int window_supervisor_task(void *arg) {
  int failures = 0;
  uint64_t code = 0;
  int h;
  int i;

  (void)arg;
  console_write_string("[WIN] waiting for the desktop task\r\n");
  while (desktop_loop_count() < 3) {
    wait_one_tick();
  }

  h = process_spawn("/APPS/WINTEST.BIN");
  failures += win_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == 0,
                         "wintest: all window checks passed");
  console_printf("[WIN] wintest exit code %llu\r\n", code);
  failures += win_expect(wait_for(cond_no_windows, 500), "windows left open were released at exit");

  // Several runs in a row would run out of the 4 window slots if any leaked.
  for (i = 0; i < 4; ++i) {
    h = process_spawn("/APPS/WINTEST.BIN");
    if (h < 0 || process_wait(h, &code) != PROCESS_OK || code != 0) {
      break;
    }
    wait_for(cond_no_windows, 500);
  }
  failures += win_expect(i == 4, "four more runs (no window slots leaked)");

  // Input events.
  h = process_spawn("/APPS/WINEV.BIN");
  failures += win_expect(h >= 0 && wait_for(cond_has_window, 500), "winev: window appeared on the desktop");
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 'x');
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, SYSWIN_KEY_LEFT);
  syswin_test_inject(SYSWIN_EV_MOUSE_DOWN, 5, 6, SYSWIN_MOD_SHIFT | SYSWIN_MOD_CTRL);
  syswin_test_inject(SYSWIN_EV_CLOSE, 0, 0, 0);
  failures += win_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == 0,
                         "winev: blocked read woke for key, mouse and close events");
  console_printf("[WIN] winev exit code %llu\r\n", code);
  failures += win_expect(wait_for(cond_no_windows, 500), "no windows left at the end");

  // The launcher behind the Notes icon starts NOTES.BIN as a program (not the built-in app).
  {
    int i2;

    int free_before = syswin_free_slots();

    app_launch_notes();
    failures += win_expect(wait_for(cond_has_window, 500), "launcher: Notes opened as a user window");
    failures += win_expect(syswin_free_slots() == free_before - 1, "launcher: it uses one window slot");
    syswin_test_inject(SYSWIN_EV_CLOSE, 0, 0, 0);
    failures += win_expect(wait_for(cond_no_windows, 500), "launcher: closing the window releases it");
    for (i2 = 0; i2 < 500 && process_live_count() != 0; ++i2) {
      wait_one_tick();                              // the detached program finishes and frees its slot
    }
    failures += win_expect(process_live_count() == 0 && !process_spawn_active("/APPS/NOTES.BIN", 0),
                           "launcher: the program exited and its slot is free");
  }

  // Paint: pick red on the palette, drag across the canvas, then press Clear.
  {
    h = process_spawn("/APPS/PAINT.BIN");
    failures += win_expect(h >= 0 && wait_for(cond_has_window, 500), "paint: window appeared");
    failures += win_expect(wait_for(cond_paint_white, 500), "paint: the white canvas was drawn");
    syswin_test_inject(SYSWIN_EV_MOUSE_DOWN, 4 + 2 * 26 + 5, 10, 0);   // the red swatch
    syswin_test_inject(SYSWIN_EV_MOUSE_UP, 4 + 2 * 26 + 5, 10, 0);
    syswin_test_inject(SYSWIN_EV_MOUSE_DOWN, 100, 100, 0);              // press on the canvas
    syswin_test_inject(SYSWIN_EV_MOUSE_MOVE, 110, 100, 0);              // drag right
    syswin_test_inject(SYSWIN_EV_MOUSE_UP, 110, 100, 0);
    failures += win_expect(wait_for(cond_paint_red, 500), "paint: dragging drew a red line");
    syswin_test_inject(SYSWIN_EV_MOUSE_MOVE, 120, 100, 0);              // pen is up: draws nothing
    syswin_test_inject(SYSWIN_EV_MOUSE_DOWN, 4 + 8 * 26 + 8 + 5, 10, 0);   // Clear
    failures += win_expect(wait_for(cond_paint_cleared, 500), "paint: Clear wiped the canvas");
    syswin_test_inject(SYSWIN_EV_CLOSE, 0, 0, 0);
    failures += win_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == 0, "paint: exited cleanly");
    failures += win_expect(wait_for(cond_no_windows, 500), "paint: no windows left");
  }

  // About: it draws its title and exits when the window closes.
  {
    h = process_spawn("/APPS/ABOUT.BIN");
    failures += win_expect(h >= 0 && wait_for(cond_has_window, 500), "about: window appeared");
    failures += win_expect(wait_for(cond_about_title, 500), "about: the title was drawn");
    syswin_test_inject(SYSWIN_EV_CLOSE, 0, 0, 0);
    failures += win_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == 0, "about: exited cleanly");
    failures += win_expect(wait_for(cond_no_windows, 500), "about: no windows left");
  }

  // Resizing: a resizable window gets a new surface of the new client size and an
  // EV_RESIZE; sizes beyond the limits are clamped to 800 x 600.
  {
    int sw = 0, sh = 0;

    h = process_spawn("/APPS/RESZT.BIN");
    failures += win_expect(h >= 0 && wait_for(cond_has_window, 500), "resize: window appeared");
    failures += win_expect(syswin_test_size(&sw, &sh) && sw == 200 && sh == 120, "resize: starts at 200x120");
    syswin_test_resize(300, 200);
    failures += win_expect(wait_for(cond_size_300, 500), "resize: surface became 300x200");
    syswin_test_resize(900, 700);
    failures += win_expect(wait_for(cond_size_800, 500), "resize: 900x700 was clamped to 800x600");
    syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 'q');
    failures += win_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == 0,
                           "resize: the program saw both EV_RESIZE events and exited cleanly");
    console_printf("[WIN] reszt exit code %llu\r\n", code);
    failures += win_expect(wait_for(cond_no_windows, 500), "resize: no windows left");
  }

  // Notes as a user program: type, save with Ctrl+S, quit; then run it again on the
  // same file, which it must load, and append to it.
  {
    static const char expect1[] = "hi\r\nyo";
    static const char expect2[] = "hi\r\nyo!";
    static char buf[64];
    const char *keys1 = "hi\nyo";
    fat_volume_t *v = fs_default_volume();
    fat_dirent_t ent;
    uint32_t got = 0;
    int ok;

    h = process_spawn_arg("/APPS/NOTES.BIN", "/NTEST.TXT");
    failures += win_expect(h >= 0 && wait_for(cond_has_window, 500), "notes: window appeared");
    for (i = 0; keys1[i] != 0; ++i) {
      syswin_test_inject(SYSWIN_EV_KEY, 0, 0, (uint32_t)(unsigned char)keys1[i]);
    }
    syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 0x13);
    syswin_test_inject(SYSWIN_EV_CLOSE, 0, 0, 0);
    failures += win_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == 0, "notes: exited cleanly");
    ok = v != 0 && fat_lookup(v, v->root_cluster, "NTEST.TXT", &ent) && ent.size == sizeof(expect1) - 1 &&
         fat_read_file(v, &ent, buf, sizeof(buf), &got) && got == sizeof(expect1) - 1 &&
         memcmp(buf, expect1, got) == 0;
    failures += win_expect(ok, "notes: typed text was saved to /NTEST.TXT (CR LF line ends)");
    wait_for(cond_no_windows, 500);

    h = process_spawn_arg("/APPS/NOTES.BIN", "/NTEST.TXT");
    failures += win_expect(h >= 0 && wait_for(cond_has_window, 500), "notes: window appeared again");
    syswin_test_inject(SYSWIN_EV_KEY, 0, 0, '!');
    syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 0x13);
    syswin_test_inject(SYSWIN_EV_CLOSE, 0, 0, 0);
    failures += win_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == 0, "notes: second run exited cleanly");
    got = 0;
    ok = v != 0 && fat_lookup(v, v->root_cluster, "NTEST.TXT", &ent) && ent.size == sizeof(expect2) - 1 &&
         fat_read_file(v, &ent, buf, sizeof(buf), &got) && got == sizeof(expect2) - 1 &&
         memcmp(buf, expect2, got) == 0;
    failures += win_expect(ok, "notes: second run loaded the file and appended to it");
    wait_for(cond_no_windows, 500);

    // Cursor movement: the file is "hi\nyo!". Up, Left, 'X', Delete, Down, 'Z' must give
    // "hX\nyoZ!" (Up keeps the column but clamps it to the short row).
    {
      static const char expect3[] = "hX\r\nyoZ!";
      static const unsigned char keys3[] = {0x1E, 0x1C, 'X', 0x7F, 0x1F, 'Z', 0x13};
      unsigned int k;

      h = process_spawn_arg("/APPS/NOTES.BIN", "/NTEST.TXT");
      failures += win_expect(h >= 0 && wait_for(cond_has_window, 500), "notes: window appeared a third time");
      for (k = 0; k < sizeof(keys3); ++k) {
        syswin_test_inject(SYSWIN_EV_KEY, 0, 0, keys3[k]);
      }
      syswin_test_inject(SYSWIN_EV_CLOSE, 0, 0, 0);
      failures += win_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == 0, "notes: third run exited cleanly");
      got = 0;
      ok = v != 0 && fat_lookup(v, v->root_cluster, "NTEST.TXT", &ent) && ent.size == sizeof(expect3) - 1 &&
           fat_read_file(v, &ent, buf, sizeof(buf), &got) && got == sizeof(expect3) - 1 &&
           memcmp(buf, expect3, got) == 0;
      failures += win_expect(ok, "notes: arrow keys, insert and Delete edited the text in place");
      wait_for(cond_no_windows, 500);
    }

    // Selection and clipboard. The file is "hX\nyoZ!". Shift+Left x2 selects "Z!", Ctrl+C
    // copies, Home goes to the row start, Ctrl+V pastes there, End goes to the end, '?'
    // is typed, Ctrl+Home goes to the top, 'A' is typed, Shift+Right selects the 'h',
    // Ctrl+X cuts it, Ctrl+End goes to the end and Ctrl+V pastes it back.
    {
      static const char expect4[] = "AX\r\nZ!yoZ!?h";
      static const struct { unsigned char ch; unsigned char mods; } keys4[] = {
        {0x1C, 1}, {0x1C, 1}, {0x03, 2}, {0x80, 0}, {0x16, 2}, {0x81, 0}, {'?', 0},
        {0x80, 2}, {'A', 0}, {0x1D, 1}, {0x18, 2}, {0x81, 2}, {0x16, 2}, {0x13, 2}};
      unsigned int k;

      h = process_spawn_arg("/APPS/NOTES.BIN", "/NTEST.TXT");
      failures += win_expect(h >= 0 && wait_for(cond_has_window, 500), "notes: window appeared a fourth time");
      for (k = 0; k < sizeof(keys4) / sizeof(keys4[0]); ++k) {
        syswin_test_inject(SYSWIN_EV_KEY, keys4[k].mods, 0, keys4[k].ch);
      }
      syswin_test_inject(SYSWIN_EV_CLOSE, 0, 0, 0);
      failures += win_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == 0, "notes: fourth run exited cleanly");
      got = 0;
      ok = v != 0 && fat_lookup(v, v->root_cluster, "NTEST.TXT", &ent) && ent.size == sizeof(expect4) - 1 &&
           fat_read_file(v, &ent, buf, sizeof(buf), &got) && got == sizeof(expect4) - 1 &&
           memcmp(buf, expect4, got) == 0;
      failures += win_expect(ok, "notes: selection, copy, cut and paste worked");
      wait_for(cond_no_windows, 500);
    }

    // Mouse: the file is "AX\nZ!yoZ!?h". Click after the 'A' (row 0, column 1), Shift+click
    // at row 1, column 1 (selects "X\nZ"), Ctrl+X cuts it, End goes to the end, Ctrl+V pastes.
    {
      static const char expect5[] = "A!yoZ!?hX\r\nZ";
      static const unsigned char keys5[] = {0x18, 0x81, 0x16, 0x13};
      unsigned int k;

      h = process_spawn_arg("/APPS/NOTES.BIN", "/NTEST.TXT");
      failures += win_expect(h >= 0 && wait_for(cond_has_window, 500), "notes: window appeared a fifth time");
      syswin_test_inject(SYSWIN_EV_MOUSE_DOWN, 16, 6, 0);
      syswin_test_inject(SYSWIN_EV_MOUSE_UP, 16, 6, 0);
      syswin_test_inject(SYSWIN_EV_MOUSE_DOWN, 16, 16, SYSWIN_MOD_SHIFT);
      syswin_test_inject(SYSWIN_EV_MOUSE_UP, 16, 16, SYSWIN_MOD_SHIFT);
      for (k = 0; k < sizeof(keys5); ++k) {
        syswin_test_inject(SYSWIN_EV_KEY, keys5[k] == 0x18 || keys5[k] == 0x16 || keys5[k] == 0x13 ? SYSWIN_MOD_CTRL : 0,
                           0, keys5[k]);
      }
      syswin_test_inject(SYSWIN_EV_CLOSE, 0, 0, 0);
      failures += win_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == 0, "notes: fifth run exited cleanly");
      got = 0;
      ok = v != 0 && fat_lookup(v, v->root_cluster, "NTEST.TXT", &ent) && ent.size == sizeof(expect5) - 1 &&
           fat_read_file(v, &ent, buf, sizeof(buf), &got) && got == sizeof(expect5) - 1 &&
           memcmp(buf, expect5, got) == 0;
      failures += win_expect(ok, "notes: click and Shift+click selected a range");
      wait_for(cond_no_windows, 500);
    }

    // Explorer: Enter opens drive 0, a click on "New Folder" makes a folder in its root,
    // Down (as many times as the folder sorts after the others) plus Delete removes it.
    {
      fat_volume_t *v0 = fs_volume(0);
      fat_iter_t *it = (fat_iter_t *)kmalloc(sizeof(*it));
      fat_dirent_t e2;
      int before = 0;
      int k;

      h = process_spawn("/APPS/EXPLORER.BIN");
      failures += win_expect(h >= 0 && wait_for(cond_has_window, 500), "explorer: window appeared");
      syswin_test_inject(SYSWIN_EV_KEY, 0, 0, '\n');            // open the first drive
      syswin_test_inject(SYSWIN_EV_MOUSE_DOWN, 60, 12, 0);      // New Folder
      failures += win_expect(v0 != 0 && it != 0 && wait_for(cond_explorer_folder, 500),
                             "explorer: New Folder created a folder on drive 0");
      if (v0 != 0 && it != 0) {
        fat_iter_open(v0, v0->root_cluster, it);
        while (fat_iter_next(it, &e2)) {            // folders that sort before "New Folder"
          if ((e2.attr & FAT_ATTR_DIR) != 0 && !str_eq_nocase(e2.name, "New Folder") &&
              name_before(e2.name, "New Folder")) {
            ++before;
          }
        }
      }
      if (it != 0) {
        kfree(it);
      }
      for (k = 0; k < before; ++k) {
        syswin_test_inject(SYSWIN_EV_KEY, 0, 0, SYSWIN_KEY_DOWN);
      }
      syswin_test_inject(SYSWIN_EV_KEY, 0, 0, SYSWIN_KEY_DELETE);
      failures += win_expect(wait_for(cond_explorer_folder_gone, 500), "explorer: Delete removed the folder");
      // Multi-select. Two folders ("New Folder", "New Folder 2"), then Shift+Down marks both
      // and Delete removes them; again, with Ctrl+click on both rows.
      syswin_test_inject(SYSWIN_EV_MOUSE_DOWN, 60, 12, 0);
      syswin_test_inject(SYSWIN_EV_MOUSE_DOWN, 60, 12, 0);
      failures += win_expect(wait_for(cond_explorer_two_folders, 500), "explorer: two folders were created");
      for (k = 0; k < before; ++k) {
        syswin_test_inject(SYSWIN_EV_KEY, 0, 0, SYSWIN_KEY_DOWN);
      }
      syswin_test_inject(SYSWIN_EV_KEY, SYSWIN_MOD_SHIFT, 0, SYSWIN_KEY_DOWN);
      syswin_test_inject(SYSWIN_EV_KEY, 0, 0, SYSWIN_KEY_DELETE);
      failures += win_expect(wait_for(cond_explorer_folder_gone, 500) && wait_for(cond_explorer_two_gone, 500),
                             "explorer: Shift+Down marked a range and Delete removed both");
      syswin_test_inject(SYSWIN_EV_MOUSE_DOWN, 60, 12, 0);
      syswin_test_inject(SYSWIN_EV_MOUSE_DOWN, 60, 12, 0);
      failures += win_expect(wait_for(cond_explorer_two_folders, 500), "explorer: two folders were created again");
      syswin_test_inject(SYSWIN_EV_MOUSE_DOWN, 100, 32 + 14 * before + 2, 0);        // plain click selects it
      syswin_test_inject(SYSWIN_EV_MOUSE_DOWN, 100, 32 + 14 * before + 2, SYSWIN_MOD_CTRL);
      syswin_test_inject(SYSWIN_EV_MOUSE_DOWN, 100, 32 + 14 * (before + 1) + 2, SYSWIN_MOD_CTRL);
      syswin_test_inject(SYSWIN_EV_KEY, 0, 0, SYSWIN_KEY_DELETE);
      failures += win_expect(wait_for(cond_explorer_folder_gone, 500) && wait_for(cond_explorer_two_gone, 500),
                             "explorer: Ctrl+click marked two items and Delete removed both");
      syswin_test_inject(SYSWIN_EV_CLOSE, 0, 0, 0);
      failures += win_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == 0, "explorer: exited cleanly");
      wait_for(cond_no_windows, 500);
    }
  }
  failures += win_expect(wait_for(cond_no_windows, 500), "no windows left at the end");
  failures += win_expect(process_live_count() == 0, "no processes left at the end");

  if (failures == 0) {
    console_write_string("[WIN] PASS: window syscalls\r\n");
  } else {
    console_printf("[WIN] FAIL: %d check(s) failed\r\n", failures);
  }
  return 0;
}

void process_window_selftest(void) {
  fs_init();
  desktop_start();
  scheduler_add_task("win-supervisor", window_supervisor_task, 0);
}
#endif

#if defined(KERNEL_USER_SELFTEST) && KERNEL_USER_SELFTEST == 9
// USER_SELFTEST=9: hardening. Bad pointers and unknown syscalls (PROBE.BIN), a stack
// overflow (STACKOVF.BIN), and a program that never gives up the CPU (SPIN.BIN).
static int hard_expect(int ok, const char *what) {
  console_printf("[HARD]  %s %s\r\n", ok ? "ok  " : "FAIL", what);
  return ok ? 0 : 1;
}

static int hard_supervisor_task(void *arg) {
  int failures = 0;
  uint64_t code = 0;
  int h, spin, hello;
  int i;

  (void)arg;
  h = process_spawn("/APPS/PROBE.BIN");
  failures += hard_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == 0,
                          "probe: every bad pointer / descriptor / syscall number failed cleanly");
  if (code != 0) {
    console_printf("[HARD] probe stopped at check %llu\r\n", code);
  }

  h = process_spawn("/APPS/STACKOVF.BIN");
  failures += hard_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK &&
                              code == PROCESS_EXIT_FAULT_BASE + 14,
                          "stack overflow: killed by a page fault");
  h = process_spawn("/APPS/STACKOVF.BIN");
  failures += hard_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK &&
                              code == PROCESS_EXIT_FAULT_BASE + 14,
                          "stack overflow again (stack and tables were freed)");

  // A program that never makes a syscall must not starve the others: a second
  // program has to start, run and finish while the first is still looping.
  spin = process_spawn("/APPS/SPIN.BIN");
  hello = process_spawn("/APPS/HELLO.BIN");
  failures += hard_expect(spin >= 0 && hello >= 0, "spin and hello spawned");
  failures += hard_expect(process_wait(hello, &code) == PROCESS_OK && code == 7, "hello finished");
  failures += hard_expect(spin >= 0 && !process_finished(spin), "spin was still looping then (preempted, not blocking)");
  failures += hard_expect(process_wait(spin, &code) == PROCESS_OK && code == 11, "spin finished by itself");

  // Faults and bad calls in a mix with healthy programs.
  {
    int a = process_spawn("/APPS/PROBE.BIN");
    int b = process_spawn("/APPS/STACKOVF.BIN");
    int c = process_spawn("/APPS/CRASH.BIN");
    int d = process_spawn("/APPS/HELLO.BIN");
    uint64_t ca = 99, cb = 0, cc = 0, cd = 0;

    failures += hard_expect(a >= 0 && b >= 0 && c >= 0 && d >= 0 &&
                                process_wait(a, &ca) == PROCESS_OK && process_wait(b, &cb) == PROCESS_OK &&
                                process_wait(c, &cc) == PROCESS_OK && process_wait(d, &cd) == PROCESS_OK &&
                                ca == 0 && cb == PROCESS_EXIT_FAULT_BASE + 14 &&
                                cc == PROCESS_EXIT_FAULT_BASE + 14 && cd == 7,
                            "mixed batch: each program got its own outcome");
  }
  for (i = 0; i < 6; ++i) {
    h = process_spawn("/APPS/STACKOVF.BIN");
    if (h < 0 || process_wait(h, &code) != PROCESS_OK || code != PROCESS_EXIT_FAULT_BASE + 14) {
      break;
    }
  }
  failures += hard_expect(i == 6, "six more stack overflows (no leak)");

  // A fault inside the kernel while serving a syscall must kill only that process.
  h = process_spawn_arg("/APPS/KFAULT.BIN", "0");
  failures += hard_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK &&
                              code == PROCESS_EXIT_FAULT_BASE + 14,
                          "kernel fault inside a syscall: process killed, kernel survives");
  h = process_spawn_arg("/APPS/KFAULT.BIN", "1");
  failures += hard_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK &&
                              code == PROCESS_EXIT_FAULT_BASE + 14,
                          "kernel fault while holding the preempt lock: process killed");
  failures += hard_expect(scheduler_preempt_level() == 0, "preempt lock was released");
  {
    int a = process_spawn_arg("/APPS/KFAULT.BIN", "1");
    int b = process_spawn("/APPS/HELLO.BIN");
    uint64_t ca = 0, cb = 0;

    failures += hard_expect(a >= 0 && b >= 0 &&
                                process_wait(a, &ca) == PROCESS_OK && process_wait(b, &cb) == PROCESS_OK &&
                                ca == PROCESS_EXIT_FAULT_BASE + 14 && cb == 7,
                            "kernel fault next to a healthy program: the other one is unaffected");
  }
  failures += hard_expect(scheduler_preempt_level() == 0, "preempt lock still released");
  failures += hard_expect(process_live_count() == 0, "nothing leaked at the end");

  if (failures == 0) {
    console_write_string("[HARD] PASS: bad pointers, unknown syscalls, stack overflow, preemption\r\n");
  } else {
    console_printf("[HARD] FAIL: %d check(s) failed\r\n", failures);
  }
  return 0;
}

void process_hardening_selftest(void) {
  fs_init();
  scheduler_add_task("hard-supervisor", hard_supervisor_task, 0);
}
#endif

#if defined(KERNEL_USER_SELFTEST) && KERNEL_USER_SELFTEST == 10
// USER_SELFTEST=10: Windows (PE) programs. WINHELLO.EXE uses the Win32 runtime (heap, files,
// console); WINBAD.EXE calls an import the runtime lacks; a file that only looks like a PE.
static int pe_expect(int ok, const char *what) {
  console_printf("[PE]  %s %s\r\n", ok ? "ok  " : "FAIL", what);
  return ok ? 0 : 1;
}

static void pe_click(int x, int y) {
  syswin_test_inject(SYSWIN_EV_MOUSE_DOWN, x, y, 0);
  syswin_test_inject(SYSWIN_EV_MOUSE_UP, x, y, 0);
}

static void pe_pause(int ticks) {
  while (ticks-- > 0) {
    wait_one_tick();
  }
}

static void pe_move(int x, int y) { syswin_test_inject(SYSWIN_EV_MOUSE_MOVE, x, y, 0); }

static int pe_wait_for(int (*cond)(void), int ticks) {
  while (ticks-- > 0 && !cond()) {
    wait_one_tick();
  }
  return cond();
}

static int cond_pe_window(void) { return syswin_test_active() > 0; }
static int cond_pe_no_windows(void) { return syswin_test_used() == 0; }
static int cond_pe_red(void) { return syswin_test_pixel(30, 30) == 0x00C82828; }          // FillRect with a solid brush
static int cond_pe_blue(void) { return syswin_test_pixel(130, 30) == 0x002828C8; }        // Rectangle filled by the selected brush
static int cond_pe_ellipse(void) { return syswin_test_pixel(230, 35) == 0x0028C828; }     // Ellipse
static int cond_pe_white(void) { return syswin_test_pixel(300, 250) == 0x00FFFFFF; }      // the class background brush
static int cond_pe_blue_text(void) { return syswin_test_count(0x000000FFu) > 40; }        // TextOut, SetTextColor
static int cond_pe_marker(void) { return syswin_test_pixel(300, 150) == 0x0028C828; }     // drawn at the click position
static int cond_pe_typed(void) { return syswin_test_count(0x00FF00FFu) > 20; }            // typed characters

// WINCTL.EXE (the standard controls): what each control draws.
static int cond_ctl_bg(void) { return syswin_test_pixel(300, 10) == 0x00C8DCFF; }          // the window class brush
static int cond_ctl_static(void) { return syswin_test_pixel(58, 12) == 0x00F0F0F0; }        // STATIC: the button face color
static int cond_ctl_edit(void) { return syswin_test_pixel(200, 20) == 0x00FFFFFF; }        // EDIT: white inside
static int cond_ctl_button(void) { return syswin_test_pixel(20, 45) == 0x00F0F0F0; }       // BUTTON
static int cond_ctl_panel(void) { return syswin_test_pixel(350, 215) == 0x00C82828; }      // a custom child window, translated
static int cond_ctl_list(void) { return syswin_test_pixel(320, 60) == 0x00FFFFFF; }        // LISTBOX
static int cond_ctl_scroll(void) { return syswin_test_pixel(342, 90) == 0x00CDCDCD; }      // its scroll bar thumb
// WINMENU.EXE: a menu bar, popups drawn over the client area and gone again.
static int cond_mnu_bar(void) { return syswin_test_pixel(300, 5) == 0x00F0F0F0; }          // the menu bar
static int cond_mnu_client(void) { return syswin_test_pixel(300, 40) == 0x00C8DCFF; }       // the client area starts below it
static int cond_mnu_red(void) { return syswin_test_pixel(20, 38) == 0x00C82828; }           // client (10,10)
static int cond_mnu_popup(void) { return syswin_test_pixel(130, 25) == 0x00F0F0F0; }        // the File popup over the client area
static int cond_mnu_popup2(void) { return syswin_test_pixel(130, 45) == 0x00F0F0F0 && syswin_test_pixel(100, 25) == 0x000078D7; }   // first item highlighted (keyboard)
static int cond_mnu_gone(void) { return syswin_test_pixel(130, 25) == 0x00C8DCFF; }         // and the client repainted after it closed
// WINDLG.EXE: a dialog from a compiled resource.
static int cond_dlg_bg(void) { return syswin_test_pixel(270, 110) == 0x00F0F0F0; }          // the dialog class background
static int cond_dlg_edit(void) { return syswin_test_pixel(150, 22) == 0x00FFFFFF; }         // its edit control
// WINCOMBO.EXE: drop-down lists over the parent window.
static int cond_cmb_field(void) { return syswin_test_pixel(100, 20) == 0x00FFFFFF; }         // the list box combo's field
static int cond_cmb_a_open(void) { return syswin_test_pixel(100, 60) == 0x00FFFFFF; }       // its list, over the parent
static int cond_cmb_a_closed(void) { return syswin_test_pixel(100, 60) == 0x00C8DCFF; }     // gone, the parent shows again
static int cond_cmb_b_open(void) { return syswin_test_pixel(300, 60) == 0x00FFFFFF; }
static int cond_cmb_b_closed(void) { return syswin_test_pixel(300, 60) == 0x00C8DCFF; }
// WINSCROLL.EXE: a vertical bar at (10,10) 17x150 and a horizontal one at (40,10) 200x17.
static int cond_scr_track(void) { return syswin_test_pixel(18, 50) == 0x00F0F0F0 && syswin_test_pixel(18, 137) != 0x00F0F0F0; }       // the thumb is at the bottom
static int cond_scr_thumb_bottom(void) { return syswin_test_pixel(18, 137) == 0x00CDCDCD; }
static int cond_scr_glyph(void) { return syswin_test_pixel(18, 12) == 0x00000000 || syswin_test_pixel(18, 11) == 0x00000000; }       // the up arrow's triangle
// WINSBAR.EXE: frame scroll bars. Client 387 x 274 in a 404 x 291 surface; the vertical bar is at x 387..404, the horizontal at y 274..291.
static int cond_sbar_drawn(void) {
  return syswin_test_pixel(395, 100) == 0x00F0F0F0 && syswin_test_pixel(395, 282) == 0x00F0F0F0 &&      // the window's two bars (and the corner)
         syswin_test_pixel(300, 60) == 0x00008000 && syswin_test_pixel(361, 60) == 0x00F0F0F0;          // a child window and its bar
}
static int cond_sbar_thumb_bottom(void) { return syswin_test_pixel(395, 245) == 0x00CDCDCD && syswin_test_pixel(395, 100) == 0x00F0F0F0; }
static int cond_sbar_marker(void) { return syswin_test_pixel(24, 200) == 0x00FF0000; }        // the program moved its marker (vertical 90, horizontal 1)
static int cond_sbar_hidden(void) { return syswin_test_pixel(395, 245) == 0x00C8DCFF; }       // the bar went away, the client area took its place
// WINBMP.EXE: blits from memory DCs and a bitmap resource onto the window.
static int cond_bmp_canvas(void) { return syswin_test_pixel(15, 15) == 0x00FF0000 && syswin_test_pixel(45, 15) == 0x000000FF; }       // a 40x30 canvas, red | blue
static int cond_bmp_pic(void) {                       // the 4x2 resource at 20x: red green blue yellow / cyan . . black
  return syswin_test_pixel(110, 20) == 0x00FF0000 && syswin_test_pixel(130, 20) == 0x0000FF00 && syswin_test_pixel(150, 20) == 0x000000FF &&
         syswin_test_pixel(170, 20) == 0x00FFFF00 && syswin_test_pixel(110, 40) == 0x0000FFFF && syswin_test_pixel(170, 40) == 0x00000000;
}
static int cond_bmp_mirror(void) {                    // the same, with a negative width: left and right swapped
  return syswin_test_pixel(110, 70) == 0x00FFFF00 && syswin_test_pixel(170, 70) == 0x00FF0000 && syswin_test_pixel(170, 90) == 0x0000FFFF;
}
static int cond_bmp_notsrc(void) { return syswin_test_pixel(15, 115) == 0x0000FFFF && syswin_test_pixel(45, 115) == 0x00FFFF00; }     // NOTSRCCOPY
static int cond_bmp_black(void) { return syswin_test_pixel(105, 105) == 0x00000000; }                                                 // PatBlt BLACKNESS
static int cond_bmp_untouched(void) { return syswin_test_pixel(15, 155) == 0x00C8DCFF; }                                              // the refused SRCINVERT drew nothing
static int cond_pe_msgbox(void) { return syswin_test_count(0x003A78D6u) > 1000; }   // the default button

static int pe_supervisor_task(void *arg) {
  int failures = 0;
  uint64_t code = 0;
  int h, a, b;
  fat_volume_t *v = fs_default_volume();
  static const uint8_t junk[200] = {'M', 'Z', 1, 2, 3};

  (void)arg;
  h = process_spawn_arg("/APPS/WINHELLO.EXE", "one two");
  failures += pe_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == 7,
                        "WINHELLO.EXE ran: console, heap and a file round trip (exit code 7)");
  h = process_spawn_arg("/APPS/WINCRT.EXE", "crt args");
  failures += pe_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == 0,
                        "WINCRT.EXE: a program built with the dynamic C runtime (/MD) passed all its checks");
  h = process_spawn("/APPS/WINCXX.EXE");
  failures += pe_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == 0,
                        "WINCXX.EXE: C++ exceptions (throw, catch, rethrow, destructors, nesting) and __try/__except/__finally");
  h = process_spawn_arg("/APPS/WINCXX.EXE", "uncaught");
  failures += pe_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == 3,
                        "an exception nothing catches ends the program (terminate, exit code 3)");
  h = process_spawn_arg("/APPS/WINCXX.EXE", "noexcept");
  failures += pe_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == 3,
                        "an exception leaving a noexcept function ends the program (terminate, exit code 3)");
  h = process_spawn("/APPS/WINTHR.EXE");
  failures += pe_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == 0,
                        "WINTHR.EXE: threads, critical sections, events, mutexes, semaphores, TLS and a shared heap passed all checks");
  h = process_spawn_arg("/APPS/WINTHR.EXE", "ret");
  failures += pe_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == 3,
                        "the main thread returned while four threads spun or waited: the program ended with its code");
  h = process_spawn_arg("/APPS/WINTHR.EXE", "exit");
  failures += pe_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == 5,
                        "a thread called exit(5): the whole program ended with 5");
  h = process_spawn_arg("/APPS/WINTHR.EXE", "fault");
  failures += pe_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == PROCESS_EXIT_FAULT_BASE + 14,
                        "a thread took a page fault: the whole program ended with the fault code");
  h = process_spawn("/APPS/WINBAD.EXE");
  failures += pe_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == 0xC0000139ULL,
                        "calling an import the runtime lacks ends the program with its own code");

  a = process_spawn("/APPS/WINHELLO.EXE");
  b = process_spawn("/APPS/HELLO.BIN");
  {
    uint64_t ca = 0, cb = 0;

    failures += pe_expect(a >= 0 && b >= 0 && process_wait(a, &ca) == PROCESS_OK && process_wait(b, &cb) == PROCESS_OK &&
                              ca == 7 && cb == 7,
                          "a PE program next to an ELF program: both finished");
  }
  if (v != 0) {
    int rc;

    failures += pe_expect(fat_write_file(v, v->root_cluster, "BADPE.EXE", junk, sizeof(junk)), "wrote a fake PE file");
    h = process_spawn("/BADPE.EXE");
    rc = h >= 0 ? process_wait(h, &code) : h;
    failures += pe_expect(rc == PROCESS_ERR_BADPE, "a file with an MZ header but no PE is refused");
    {
      fat_dirent_t e;

      if (fat_lookup(v, v->root_cluster, "BADPE.EXE", &e)) {
        fat_delete(v, &e);
      }
    }
  }
  // SSE: three copies of a long floating-point computation run side by side; each must get
  // the same answer twice (their SSE registers must not leak into each other).
  {
    int f1 = process_spawn_arg("/APPS/WINFP.EXE", "a");
    int f2 = process_spawn_arg("/APPS/WINFP.EXE", "b");
    int f3 = process_spawn_arg("/APPS/WINFP.EXE", "c");
    uint64_t c1 = 9, c2 = 9, c3 = 9;

    failures += pe_expect(f1 >= 0 && f2 >= 0 && f3 >= 0 && process_wait(f1, &c1) == PROCESS_OK &&
                              process_wait(f2, &c2) == PROCESS_OK && process_wait(f3, &c3) == PROCESS_OK && c1 == 0 && c2 == 0 && c3 == 0,
                          "floating point: three SSE programs at once kept their own registers");
  }

  // MessageBoxA: a window of its own with buttons; Enter presses the default one.
  while (desktop_loop_count() < 3) {
    wait_one_tick();
  }
  h = process_spawn("/APPS/WINMSG.EXE");
  failures += pe_expect(h >= 0 && pe_wait_for(cond_pe_window, 500), "WINMSG.EXE: the message box window appeared");
  failures += pe_expect(pe_wait_for(cond_pe_msgbox, 500), "MessageBoxA drew its buttons");
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, '\n');
  failures += pe_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == 6, "MessageBoxA returned IDYES when Enter was pressed");
  failures += pe_expect(pe_wait_for(cond_pe_no_windows, 500), "the message box window was released");

  // Standard controls: child windows, buttons, edit, list box, focus, Tab and Enter.
  while (desktop_loop_count() < 3) {
    wait_one_tick();
  }
  h = process_spawn("/APPS/WINCTL.EXE");
  failures += pe_expect(h >= 0 && pe_wait_for(cond_pe_window, 500), "WINCTL.EXE: its window appeared");
  failures += pe_expect(pe_wait_for(cond_ctl_bg, 500), "controls: the parent window was erased with its class brush");
  failures += pe_expect(pe_wait_for(cond_ctl_static, 500), "controls: STATIC drew its background");
  failures += pe_expect(pe_wait_for(cond_ctl_edit, 500), "controls: EDIT drew white inside");
  failures += pe_expect(pe_wait_for(cond_ctl_button, 500), "controls: BUTTON drew its face");
  failures += pe_expect(pe_wait_for(cond_ctl_panel, 500), "controls: a custom child window painted at its place (device context origin)");
  failures += pe_expect(pe_wait_for(cond_ctl_list, 500), "controls: LISTBOX drew white inside");
  failures += pe_expect(pe_wait_for(cond_ctl_scroll, 500), "controls: LISTBOX drew its scroll bar thumb");
  pe_click(50, 52);                                                      // OK
  pe_click(140, 52);                                                     // the disabled button
  pe_click(15, 78);                                                      // the check box
  pe_click(15, 123);                                                     // the second radio button
  pe_click(80, 17);                                                      // into the edit control, after the 'a'
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 'x');
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 'y');
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 'z');
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 8);                           // backspace
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 9);                         // Tab: to the OK button
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, ' ');                          // space presses it
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 10);                         // Enter presses the default button
  pe_click(50, 170);                                                     // the ES_NUMBER edit control
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, '4');
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 'a');
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, '2');
  pe_click(210, 72);                                                     // list item 2
  pe_click(210, 84);                                                     // list item 3, then a double click on it
  syswin_test_inject(SYSWIN_EV_MOUSE_DOWN, 210, 84, 0);
  syswin_test_inject(SYSWIN_EV_MOUSE_DOUBLE, 210, 84, 0);
  syswin_test_inject(SYSWIN_EV_MOUSE_UP, 210, 84, 0);
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 0x1F);                        // Down arrow
  pe_click(310, 200);                                                    // the custom child window
  for (a = 0; a < 40; ++a) {
    wait_one_tick();
  }
  syswin_test_inject(SYSWIN_EV_CLOSE, 0, 0, 0);
  code = 0xFFFFFFFFULL;
  failures += pe_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == 0,
                        "controls: every check in the program passed (exit code = failed checks bit mask)");
  if (code != 0) {
    console_printf("[PE] WINCTL failed checks mask: 0x%x\r\n", (unsigned)code);
  }
  failures += pe_expect(pe_wait_for(cond_pe_no_windows, 500), "the controls window was released");

  // Menus: bar, popups, submenu, Alt+letter, accelerator, TrackPopupMenu.
  while (desktop_loop_count() < 3) {
    wait_one_tick();
  }
  h = process_spawn("/APPS/WINMENU.EXE");
  failures += pe_expect(h >= 0 && pe_wait_for(cond_pe_window, 500), "WINMENU.EXE: its window appeared");
  failures += pe_expect(pe_wait_for(cond_mnu_bar, 500), "menus: the menu bar was drawn");
  failures += pe_expect(pe_wait_for(cond_mnu_client, 500), "menus: the client area starts below the bar");
  failures += pe_expect(pe_wait_for(cond_mnu_red, 500), "menus: client drawing is moved down by the bar");
  pe_click(10, 8);                                                       // File
  failures += pe_expect(pe_wait_for(cond_mnu_popup, 500), "menus: the File popup opened over the client area");
  pe_move(30, 65);                                                       // the disabled Save: clicking it does nothing
  pe_click(30, 65);
  pe_pause(4);
  pe_move(30, 47);                                                       // Open
  pe_click(30, 47);
  failures += pe_expect(pe_wait_for(cond_mnu_gone, 500), "menus: choosing an item closed the popup and the client was painted again");
  pe_click(70, 8);                                                       // Edit
  pe_pause(4);
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 0x1F);                        // Down x3: Cut, Copy, More
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 0x1F);
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 0x1F);
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 0x1D);                        // Right: the submenu
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 10);                          // Enter: Deep
  pe_pause(8);
  syswin_test_inject(SYSWIN_EV_KEY, 4, 0, 'f');                         // Alt+F opens File
  pe_pause(4);
  failures += pe_expect(pe_wait_for(cond_mnu_popup2, 500), "menus: Alt+F opened the File popup");
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 27);                          // Esc closes it, no command
  failures += pe_expect(pe_wait_for(cond_mnu_gone, 500), "menus: Escape closed it");
  syswin_test_inject(SYSWIN_EV_KEY, 2, 0, 14);                          // Ctrl+N: the accelerator
  pe_pause(8);
  pe_click(200, 168);                                                    // client (200,150): TrackPopupMenu there
  pe_pause(8);
  pe_move(210, 197);                                                     // Beta
  pe_click(210, 197);
  pe_pause(8);
  syswin_test_inject(SYSWIN_EV_CLOSE, 0, 0, 0);
  code = 0xFFFFFFFFULL;
  failures += pe_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == 0,
                        "menus: every check in the program passed (exit code = failed checks bit mask)");
  if (code != 0) {
    console_printf("[PE] WINMENU failed checks mask: 0x%x\r\n", (unsigned)code);
  }
  failures += pe_expect(pe_wait_for(cond_pe_no_windows, 500), "the menus window was released");

  // Dialogs and resources: a DIALOGEX from the program's .rsrc, then one built from memory.
  while (desktop_loop_count() < 3) {
    wait_one_tick();
  }
  h = process_spawn("/APPS/WINDLG.EXE");
  failures += pe_expect(h >= 0 && pe_wait_for(cond_pe_window, 500), "WINDLG.EXE: the dialog window appeared");
  failures += pe_expect(pe_wait_for(cond_dlg_bg, 500), "dialogs: the dialog was painted (class background)");
  failures += pe_expect(pe_wait_for(cond_dlg_edit, 500), "dialogs: the edit control from the resource was created and painted");
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 'x');                         // the first control has the focus
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 'y');
  pe_click(30, 55);                                                      // the check box
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 9);                           // Tab: to OK
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, ' ');                         // Space presses it: the dialog ends
  pe_pause(40);
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 10);                          // Enter in the dialog built from memory
  code = 0xFFFFFFFFULL;
  failures += pe_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == 0,
                        "dialogs and resources: every check in the program passed (exit code = failed checks bit mask)");
  if (code != 0) {
    console_printf("[PE] WINDLG failed checks mask: 0x%x\r\n", (unsigned)code);
  }
  failures += pe_expect(pe_wait_for(cond_pe_no_windows, 500), "the dialog windows were released");

  // Combo boxes: a CBS_DROPDOWNLIST and an editable CBS_DROPDOWN.
  while (desktop_loop_count() < 3) {
    wait_one_tick();
  }
  h = process_spawn("/APPS/WINCOMBO.EXE");
  failures += pe_expect(h >= 0 && pe_wait_for(cond_pe_window, 500), "WINCOMBO.EXE: its window appeared");
  failures += pe_expect(pe_wait_for(cond_cmb_field, 500), "combo boxes: the field was drawn");
  failures += pe_expect(pe_wait_for(cond_cmb_a_closed, 500), "combo boxes: closed, the window is only the field's height (nothing under it)");
  pe_click(60, 20);                                                      // open the first one
  failures += pe_expect(pe_wait_for(cond_cmb_a_open, 500), "combo boxes: the list dropped over the parent");
  pe_click(60, 62);                                                      // choose Blue
  failures += pe_expect(pe_wait_for(cond_cmb_a_closed, 500), "combo boxes: choosing an item closed the list and the parent was painted again");
  pe_click(250, 20);                                                     // into the editable one
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 'z');
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 'z');
  pe_pause(4);
  pe_click(342, 20);                                                     // its button
  failures += pe_expect(pe_wait_for(cond_cmb_b_open, 500), "combo boxes: the editable one dropped its list");
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 0x1F);                        // Down, Down: alpha, beta
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 0x1F);
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 10);                          // Enter accepts
  failures += pe_expect(pe_wait_for(cond_cmb_b_closed, 500), "combo boxes: Enter closed the list");
  pe_click(60, 20);                                                      // the first one again
  pe_pause(6);
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 0x1F);                        // Down while dropped
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 27);                          // Escape cancels: back to Blue
  pe_pause(6);
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 0x1F);                        // Down while closed: Yellow
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 'c');                         // type-ahead: Cyan
  pe_pause(8);
  syswin_test_inject(SYSWIN_EV_CLOSE, 0, 0, 0);
  code = 0xFFFFFFFFULL;
  failures += pe_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == 0,
                        "combo boxes: every check in the program passed (exit code = failed checks bit mask)");
  if (code != 0) {
    console_printf("[PE] WINCOMBO failed checks mask: 0x%x\r\n", (unsigned)code);
  }
  failures += pe_expect(pe_wait_for(cond_pe_no_windows, 500), "the combo box window was released");

  // Scroll bars: arrows, the track, dragging the thumb, a disabled arrow and the keyboard.
  while (desktop_loop_count() < 3) {
    wait_one_tick();
  }
  h = process_spawn("/APPS/WINSCROLL.EXE");
  failures += pe_expect(h >= 0 && pe_wait_for(cond_pe_window, 500), "WINSCROLL.EXE: its window appeared");
  failures += pe_expect(pe_wait_for(cond_scr_glyph, 500), "scroll bars: the control was drawn (arrow glyph)");
  pe_click(18, 152);                                                     // the down arrow: line down
  pe_pause(4);
  pe_click(18, 110);                                                     // the track below the thumb: page down
  pe_pause(4);
  syswin_test_inject(SYSWIN_EV_MOUSE_DOWN, 18, 44, 0);                  // take hold of the thumb ...
  pe_move(18, 100);
  pe_move(18, 160);                                                      // ... and drag it to the bottom
  syswin_test_inject(SYSWIN_EV_MOUSE_UP, 18, 160, 0);
  failures += pe_expect(pe_wait_for(cond_scr_thumb_bottom, 500), "scroll bars: the thumb followed the drag to the bottom");
  failures += pe_expect(pe_wait_for(cond_scr_track, 500), "scroll bars: the track shows where the thumb was");
  pe_click(18, 18);                                                      // the up arrow: line up
  pe_pause(4);
  pe_click(235, 18);                                                     // the horizontal bar's right arrow
  pe_pause(4);
  pe_click(190, 18);                                                     // its track right of the thumb: page right
  pe_pause(4);
  pe_click(48, 18);                                                      // its left arrow is disabled: nothing happens
  pe_pause(4);
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 0x80);                        // keyboard on the focused vertical bar: Home, PageDown, Down, End, Up
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 0x83);
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 0x1F);
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 0x81);
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 0x1E);
  pe_pause(8);
  syswin_test_inject(SYSWIN_EV_CLOSE, 0, 0, 0);
  code = 0xFFFFFFFFULL;
  failures += pe_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == 0,
                        "scroll bars: every check in the program passed (exit code = failed checks bit mask)");
  if (code != 0) {
    console_printf("[PE] WINSCROLL failed checks mask: 0x%x\r\n", (unsigned)code);
  }
  failures += pe_expect(pe_wait_for(cond_pe_no_windows, 500), "the scroll bar window was released");

  // Scroll bars in the frame of a window and of a child window.
  while (desktop_loop_count() < 3) {
    wait_one_tick();
  }
  h = process_spawn("/APPS/WINSBAR.EXE");
  failures += pe_expect(h >= 0 && pe_wait_for(cond_pe_window, 500), "WINSBAR.EXE: its window appeared");
  failures += pe_expect(pe_wait_for(cond_sbar_drawn, 500), "window scroll bars: both bars of the window and the bar of a child window were drawn");
  pe_click(395, 266);                                                    // the vertical bar's down arrow
  pe_pause(4);
  pe_click(395, 200);                                                    // its track below the thumb
  pe_pause(4);
  syswin_test_inject(SYSWIN_EV_MOUSE_DOWN, 395, 55, 0);                 // the thumb ...
  pe_move(395, 150);
  pe_move(395, 270);                                                     // ... dragged to the bottom
  syswin_test_inject(SYSWIN_EV_MOUSE_UP, 395, 270, 0);
  failures += pe_expect(pe_wait_for(cond_sbar_thumb_bottom, 500), "window scroll bars: the thumb followed the drag");
  pe_click(380, 282);                                                    // the horizontal bar's right arrow
  failures += pe_expect(pe_wait_for(cond_sbar_marker, 500), "window scroll bars: the program scrolled its content (marker moved)");
  pe_click(395, 282);                                                    // the corner: nothing
  pe_click(100, 100);                                                    // the client area: reaches the program
  pe_click(361, 112);                                                    // the child window's own bar: line down
  pe_pause(6);
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 'h');                         // the program hides its vertical bar
  failures += pe_expect(pe_wait_for(cond_sbar_hidden, 500), "window scroll bars: hiding a bar gave its room back to the client area");
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 's');                         // and shows it again
  failures += pe_expect(pe_wait_for(cond_sbar_thumb_bottom, 500), "window scroll bars: shown again with its position kept");
  pe_pause(4);
  syswin_test_inject(SYSWIN_EV_CLOSE, 0, 0, 0);
  code = 0xFFFFFFFFULL;
  failures += pe_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == 0,
                        "window scroll bars: every check in the program passed (exit code = failed checks bit mask)");
  if (code != 0) {
    console_printf("[PE] WINSBAR failed checks mask: 0x%x\r\n", (unsigned)code);
  }
  failures += pe_expect(pe_wait_for(cond_pe_no_windows, 500), "the frame scroll bar window was released");

  // Bitmaps: memory DCs, raster operations, DIBs and a bitmap resource.
  while (desktop_loop_count() < 3) {
    wait_one_tick();
  }
  h = process_spawn("/APPS/WINBMP.EXE");
  failures += pe_expect(h >= 0 && pe_wait_for(cond_pe_window, 500), "WINBMP.EXE: its window appeared");
  failures += pe_expect(pe_wait_for(cond_bmp_canvas, 500), "bitmaps: BitBlt of a memory DC's bitmap onto the window");
  failures += pe_expect(pe_wait_for(cond_bmp_pic, 500), "bitmaps: a bitmap resource stretched 20 times with StretchBlt");
  failures += pe_expect(pe_wait_for(cond_bmp_mirror, 500), "bitmaps: a negative width mirrors the picture");
  failures += pe_expect(pe_wait_for(cond_bmp_notsrc, 500), "bitmaps: NOTSRCCOPY");
  failures += pe_expect(pe_wait_for(cond_bmp_black, 500), "bitmaps: PatBlt BLACKNESS");
  failures += pe_expect(cond_bmp_untouched(), "bitmaps: a blit that would read the window is refused and draws nothing");
  syswin_test_inject(SYSWIN_EV_CLOSE, 0, 0, 0);
  code = 0xFFFFFFFFULL;
  failures += pe_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == 0,
                        "bitmaps: every check in the program passed (exit code = failed checks bit mask)");
  if (code != 0) {
    console_printf("[PE] WINBMP failed checks mask: 0x%x\r\n", (unsigned)code);
  }
  failures += pe_expect(pe_wait_for(cond_pe_no_windows, 500), "the bitmap window was released");

  // A windowed program: WinMain, window class, WM_PAINT with GDI, input and a timer. The desktop
  // task must be running (it is, as the idle callback).
  while (desktop_loop_count() < 3) {
    wait_one_tick();
  }
  h = process_spawn("/APPS/WINGUI.EXE");
  failures += pe_expect(h >= 0 && pe_wait_for(cond_pe_window, 500), "WINGUI.EXE: its window appeared");
  failures += pe_expect(pe_wait_for(cond_pe_white, 500), "WM_PAINT: the class background brush erased the window");
  failures += pe_expect(pe_wait_for(cond_pe_red, 500), "GDI: FillRect with a solid brush");
  failures += pe_expect(pe_wait_for(cond_pe_blue, 500), "GDI: Rectangle filled with the selected brush");
  failures += pe_expect(pe_wait_for(cond_pe_ellipse, 500), "GDI: Ellipse");
  failures += pe_expect(pe_wait_for(cond_pe_blue_text, 500), "GDI: TextOut with SetTextColor");
  syswin_test_inject(SYSWIN_EV_MOUSE_DOWN, 300, 150, 0);                // WM_LBUTTONDOWN
  syswin_test_inject(SYSWIN_EV_MOUSE_UP, 300, 150, 0);
  failures += pe_expect(pe_wait_for(cond_pe_marker, 500), "WM_LBUTTONDOWN reached the window procedure and InvalidateRect repainted");
  syswin_test_inject(SYSWIN_EV_MOUSE_DOWN, 320, 160, 0);
  syswin_test_inject(SYSWIN_EV_MOUSE_UP, 320, 160, 0);
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 'a');                         // WM_KEYDOWN, WM_CHAR, WM_KEYUP
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 'b');
  syswin_test_inject(SYSWIN_EV_KEY, 0, 0, 'c');
  failures += pe_expect(pe_wait_for(cond_pe_typed, 500), "WM_CHAR delivered the typed characters");
  for (a = 0; a < 20; ++a) {                                             // let the 40 ms timer fire at least once
    wait_one_tick();
  }
  syswin_test_inject(SYSWIN_EV_CLOSE, 0, 0, 0);                         // WM_CLOSE -> DestroyWindow -> WM_DESTROY -> PostQuitMessage
  failures += pe_expect(h >= 0 && process_wait(h, &code) == PROCESS_OK && code == 123,
                        "exit code 123 = 2 clicks, 3 characters, timer fired (WM_QUIT carried it out)");
  failures += pe_expect(pe_wait_for(cond_pe_no_windows, 500), "the window was released at exit");
  for (a = 0; a < 6; ++a) {
    h = process_spawn("/APPS/WINHELLO.EXE");
    if (h < 0 || process_wait(h, &code) != PROCESS_OK || code != 7) {
      break;
    }
  }
  failures += pe_expect(a == 6, "six more runs (the 2 MB heap and the image were freed each time)");
  failures += pe_expect(process_live_count() == 0, "nothing leaked at the end");

  if (failures == 0) {
    console_write_string("[PE] PASS: Windows programs\r\n");
  } else {
    console_printf("[PE] FAIL: %d check(s) failed\r\n", failures);
  }
  return 0;
}

void process_pe_selftest(void) {
  fs_init();
  desktop_start();
  scheduler_add_task("pe-supervisor", pe_supervisor_task, 0);
}
#endif


#if defined(KERNEL_USER_SELFTEST) && KERNEL_USER_SELFTEST == 11
// USER_SELFTEST=11: not a test. Starts the Windows demo programs on the desktop and leaves them
// there, to look at in a screenshot (scripts/qemu-screenshot.py, wait ~25 s).
static int pe_demo_task(void *arg) {
  (void)arg;
  while (desktop_loop_count() < 3) {
    wait_one_tick();
  }
  process_spawn("/APPS/WINCTL.EXE");
  for (;;) {
    wait_one_tick();
  }
  return 0;
}

void process_pe_demo(void) {
  fs_init();
  desktop_start();
  scheduler_add_task("pe-demo", pe_demo_task, 0);
}
#endif
