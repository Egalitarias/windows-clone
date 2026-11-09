#include "console.h"
#include "paging.h"
#include "irqlock.h"

#define PAGE_SIZE_BYTES 4096ULL
#define PAGE_PRESENT    0x001ULL
#define PAGE_WRITE      0x002ULL
#define PAGE_USER       0x004ULL
#define PAGE_NX         (1ULL << 63)
#define MANAGED_MEMORY_LIMIT (PMM_MAX_PAGES * PAGE_SIZE_BYTES)

#define EFI_MEMORY_TYPE_LOADER_CODE         1U
#define EFI_MEMORY_TYPE_LOADER_DATA         2U
#define EFI_MEMORY_TYPE_BOOT_SERVICES_CODE   3U
#define EFI_MEMORY_TYPE_BOOT_SERVICES_DATA   4U
#define EFI_MEMORY_TYPE_RUNTIME_SERVICES_CODE 5U
#define EFI_MEMORY_TYPE_RUNTIME_SERVICES_DATA 6U
#define EFI_MEMORY_TYPE_CONVENTIONAL         7U
#define EFI_MEMORY_TYPE_ACPI_RECLAIM         9U
#define EFI_MEMORY_TYPE_ACPI_NVS            10U
#define EFI_MEMORY_TYPE_MMIO                11U
#define EFI_MEMORY_TYPE_MMIO_PORT           12U
#define EFI_MEMORY_TYPE_PAL_CODE            13U
#define EFI_MEMORY_TYPE_PERSISTENT          14U

static uint64_t *g_pml4 = 0;
static uint64_t *g_page_table_pool = 0;
static uint64_t g_page_table_pool_pages = 0;
static uint64_t g_page_table_pool_used = 0;

static uint64_t align_down(uint64_t value, uint64_t alignment) {
  return value & ~(alignment - 1ULL);
}

static uint64_t align_up(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1ULL) & ~(alignment - 1ULL);
}

static uint64_t page_index(uint64_t address, uint64_t shift) {
  return (address >> shift) & 0x1FFULL;
}

static uint64_t *alloc_page_table(void) {
  uint64_t *table;
  uint64_t i;

  if (g_page_table_pool == 0 || g_page_table_pool_used >= g_page_table_pool_pages) {
    return 0;
  }

  table = (uint64_t *)((uint8_t *)g_page_table_pool + (g_page_table_pool_used * PAGE_SIZE_BYTES));
  for (i = 0; i < 512; ++i) {
    table[i] = 0;
  }
  ++g_page_table_pool_used;
  return table;
}

static uint64_t *ensure_table(uint64_t *parent, uint64_t index, uint64_t extra_flags) {
  uint64_t entry;
  uint64_t *child;

  entry = parent[index];
  if ((entry & PAGE_PRESENT) != 0) {
    // A user mapping needs U/S set at every level of the walk. Setting it on a
    // shared upper-level entry is safe: kernel leaf pages stay supervisor-only
    // because their own PTEs lack the U bit.
    parent[index] = entry | extra_flags;
    return (uint64_t *)(uintptr_t)(entry & 0x000FFFFFFFFFF000ULL);
  }

  child = alloc_page_table();
  if (child == 0) {
    return 0;
  }

  parent[index] = ((uint64_t)(uintptr_t)child) | PAGE_PRESENT | PAGE_WRITE | extra_flags;
  return child;
}

static int map_page_virt(uint64_t virt, uint64_t phys, uint64_t flags) {
  uint64_t *pdpt;
  uint64_t *pd;
  uint64_t *pt;
  uint64_t pml4_index;
  uint64_t pdpt_index;
  uint64_t pd_index;
  uint64_t pt_index;
  uint64_t extra;

  if (g_pml4 == 0) {
    return 0;
  }

  pml4_index = page_index(virt, 39);
  pdpt_index = page_index(virt, 30);
  pd_index = page_index(virt, 21);
  pt_index = page_index(virt, 12);

  extra = flags & PAGE_USER;
  pdpt = ensure_table(g_pml4, pml4_index, extra);
  if (pdpt == 0) {
    return 0;
  }

  pd = ensure_table(pdpt, pdpt_index, extra);
  if (pd == 0) {
    return 0;
  }

  pt = ensure_table(pd, pd_index, extra);
  if (pt == 0) {
    return 0;
  }

  pt[pt_index] = (phys & 0x000FFFFFFFFFF000ULL) | flags | PAGE_PRESENT;
  return 1;
}

static int clear_page_virt(uint64_t virt) {
  uint64_t *pdpt;
  uint64_t *pd;
  uint64_t *pt;
  uint64_t pml4_index;
  uint64_t pdpt_index;
  uint64_t pd_index;
  uint64_t pt_index;

  if (g_pml4 == 0) {
    return 0;
  }

  pml4_index = page_index(virt, 39);
  pdpt_index = page_index(virt, 30);
  pd_index = page_index(virt, 21);
  pt_index = page_index(virt, 12);

  uint64_t entry;

  entry = g_pml4[pml4_index];
  if ((entry & PAGE_PRESENT) == 0) {
    return 1; // already unmapped
  }
  pdpt = (uint64_t *)(uintptr_t)(entry & 0x000FFFFFFFFFF000ULL);

  entry = pdpt[pdpt_index];
  if ((entry & PAGE_PRESENT) == 0) {
    return 1;
  }
  pd = (uint64_t *)(uintptr_t)(entry & 0x000FFFFFFFFFF000ULL);

  entry = pd[pd_index];
  if ((entry & PAGE_PRESENT) == 0) {
    return 1;
  }
  pt = (uint64_t *)(uintptr_t)(entry & 0x000FFFFFFFFFF000ULL);

  pt[pt_index] = 0;
  __asm__ volatile("invlpg (%0)" : : "r"((void *)(uintptr_t)virt) : "memory");
  return 1;
}

static int map_range_virt(uint64_t virt_start, uint64_t phys_start, uint64_t size, uint64_t flags) {
  uint64_t addr;
  uint64_t phys;
  uint64_t end;

  if (size == 0) {
    return 1;
  }

  addr = align_down(virt_start, PAGE_SIZE_BYTES);
  phys = align_down(phys_start, PAGE_SIZE_BYTES);
  if (addr == 0) {
    addr = PAGE_SIZE_BYTES;
  }
  if (phys == 0) {
    phys = PAGE_SIZE_BYTES;
  }
  end = align_up(virt_start + size, PAGE_SIZE_BYTES);
  while (addr < end) {
    if (!map_page_virt(addr, phys, flags)) {
      console_write_string("    Paging: map page failed\r\n");
      return 0;
    }
    addr += PAGE_SIZE_BYTES;
    phys += PAGE_SIZE_BYTES;
  }

  return 1;
}

int paging_init(const EFI_MEMORY_DESCRIPTOR *memory_map,
                uint64_t kernel_image_base,
                uint64_t kernel_image_size,
                uint64_t desc_size,
                uint64_t entries) {
  uint64_t stack_pointer;
  uint64_t stack_region_start;
  uint64_t stack_region_size;

  if (memory_map == 0 || desc_size == 0 || entries == 0) {
    return 0;
  }

  g_pml4 = alloc_page_table();
  if (g_pml4 == 0) {
    console_write_string("    Paging: failed to allocate PML4\r\n");
    return 0;
  }

  if (kernel_image_base != 0 && kernel_image_size != 0) {
    if (!map_range_virt(kernel_image_base, kernel_image_base, kernel_image_size, PAGE_WRITE)) {
      console_write_string("    Paging: kernel identity map failed\r\n");
      return 0;
    }
    if (!map_range_virt(KERNEL_HIGH_HALF_BASE,
                        kernel_image_base,
                        kernel_image_size,
                        PAGE_WRITE)) {
      console_write_string("    Paging: kernel higher-half map failed\r\n");
      return 0;
    }
  }

  __asm__ volatile("mov %%rsp, %0" : "=r"(stack_pointer));
  stack_region_start = align_down(stack_pointer - (PAGE_SIZE_BYTES * 4ULL), PAGE_SIZE_BYTES);
  stack_region_size = PAGE_SIZE_BYTES * 16ULL;
  if (!map_range_virt(stack_region_start, stack_region_start, stack_region_size, PAGE_WRITE)) {
    console_write_string("    Paging: stack map failed\r\n");
    return 0;
  }

  // The page table pool lives at an arbitrary physical address chosen by UEFI
  // AllocatePages.  After CR3 is loaded, any modification of the page tables
  // (e.g. via paging_map_range) dereferences pool pointers.  Map the pool
  // itself before activating CR3 so those accesses don't page-fault.
  if (g_page_table_pool != 0 && g_page_table_pool_pages != 0) {
    uint64_t pool_phys = (uint64_t)(uintptr_t)g_page_table_pool;
    if (!map_range_virt(pool_phys, pool_phys, g_page_table_pool_pages * PAGE_SIZE_BYTES, PAGE_WRITE)) {
      console_write_string("    Paging: pool map failed\r\n");
      return 0;
    }
  }

  __asm__ volatile("mov %0, %%cr3" : : "r"((uint64_t)(uintptr_t)g_pml4) : "memory");
  return 1;
}

void paging_set_page_table_pool(void *pool, uint64_t size) {
  g_page_table_pool = (uint64_t *)pool;
  g_page_table_pool_pages = size / PAGE_SIZE_BYTES;
  g_page_table_pool_used = 0;
}

static int paging_map_range_locked(uint64_t phys_start, uint64_t size) {
  return map_range_virt(phys_start, phys_start, size, PAGE_WRITE);
}

static int paging_map_mmio_locked(uint64_t phys_start, uint64_t size) {
  // PWT | PCD selects the strong-uncached PAT entry.
  return map_range_virt(phys_start, phys_start, size, PAGE_WRITE | 0x08ULL | 0x10ULL);
}

static void paging_unmap_identity_range_locked(uint64_t phys_start, uint64_t size) {
  uint64_t addr;
  uint64_t end;

  if (size == 0) {
    return;
  }

  addr = align_down(phys_start, PAGE_SIZE_BYTES);
  end = align_up(phys_start + size, PAGE_SIZE_BYTES);
  while (addr < end) {
    clear_page_virt(addr);
    addr += PAGE_SIZE_BYTES;
  }
}

static int paging_map_user_locked(uint64_t virt_start, uint64_t phys_start, uint64_t size, int writable) {
  uint64_t flags = PAGE_USER | (writable ? PAGE_WRITE : 0ULL);
  uint64_t offset;
  uint64_t total = align_up(size, PAGE_SIZE_BYTES);

  if (virt_start != align_down(virt_start, PAGE_SIZE_BYTES) ||
      phys_start != align_down(phys_start, PAGE_SIZE_BYTES) ||
      virt_start >= USER_VA_LIMIT || total > USER_VA_LIMIT - virt_start) {
    return 0;
  }

  // NX is not used here: EFER.NXE is not guaranteed to be set by the firmware,
  // and a set bit 63 with NXE clear is a reserved-bit page fault.
  for (offset = 0; offset < total; offset += PAGE_SIZE_BYTES) {
    if (!map_page_virt(virt_start + offset, phys_start + offset, flags)) {
      return 0;
    }
    __asm__ volatile("invlpg (%0)" : : "r"((void *)(uintptr_t)(virt_start + offset)) : "memory");
  }
  return 1;
}

// ---- Per-process address spaces ----------------------------------------------
// A process PML4 starts as a copy of the kernel PML4, so every process shares the
// kernel mappings (identity map, higher half, MMIO) through the same lower-level
// tables, and then owns private tables for its user slots. Those tables come from
// the physical allocator (the kernel's own table pool cannot be freed), so a
// process can be torn down completely.

static uint64_t read_cr3(void) {
  uint64_t value;

  __asm__ volatile("mov %%cr3, %0" : "=r"(value));
  return value & 0x000FFFFFFFFFF000ULL;
}

uint64_t paging_kernel_cr3(void) {
  return (uint64_t)(uintptr_t)g_pml4;
}

static void paging_activate_locked(uint64_t cr3) {
  if (g_pml4 == 0) {
    return;
  }
  if (cr3 == 0) {
    cr3 = (uint64_t)(uintptr_t)g_pml4;
  }
  if (read_cr3() != cr3) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
  }
}

// One zeroed page-table page, reachable through the identity map.
static uint64_t *alloc_user_table(void) {
  uint64_t phys = pmm_alloc_page();
  uint64_t i;
  uint64_t *table;

  if (phys == 0) {
    return 0;
  }
  if (!map_range_virt(phys, phys, PAGE_SIZE_BYTES, PAGE_WRITE)) {
    pmm_free_page(phys);
    return 0;
  }
  table = (uint64_t *)(uintptr_t)phys;
  for (i = 0; i < 512; ++i) {
    table[i] = 0;
  }
  return table;
}

static uint64_t paging_create_address_space_locked(void) {
  uint64_t *pml4;
  uint64_t i;

  if (g_pml4 == 0) {
    return 0;
  }
  pml4 = alloc_user_table();
  if (pml4 == 0) {
    return 0;
  }
  for (i = 0; i < 512; ++i) {
    pml4[i] = g_pml4[i];
  }
  return (uint64_t)(uintptr_t)pml4;
}

static uint64_t *user_table_next(uint64_t *parent, uint64_t index) {
  uint64_t entry = parent[index];
  uint64_t *child;

  if ((entry & PAGE_PRESENT) != 0) {
    return (uint64_t *)(uintptr_t)(entry & 0x000FFFFFFFFFF000ULL);
  }
  child = alloc_user_table();
  if (child == 0) {
    return 0;
  }
  parent[index] = (uint64_t)(uintptr_t)child | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
  return child;
}

static int paging_map_user_in_locked(uint64_t cr3, uint64_t virt_start, uint64_t phys_start, uint64_t size, int writable) {
  uint64_t *pml4 = (uint64_t *)(uintptr_t)cr3;
  uint64_t flags = PAGE_USER | PAGE_PRESENT | (writable ? PAGE_WRITE : 0ULL);
  uint64_t total = align_up(size, PAGE_SIZE_BYTES);
  uint64_t offset;

  if (cr3 == 0 || cr3 == (uint64_t)(uintptr_t)g_pml4 ||
      virt_start != align_down(virt_start, PAGE_SIZE_BYTES) ||
      phys_start != align_down(phys_start, PAGE_SIZE_BYTES) ||
      virt_start >= USER_VA_LIMIT || total > USER_VA_LIMIT - virt_start) {
    return 0;
  }

  for (offset = 0; offset < total; offset += PAGE_SIZE_BYTES) {
    uint64_t virt = virt_start + offset;
    uint64_t slot = page_index(virt, 39);
    uint64_t *pdpt;
    uint64_t *pd;
    uint64_t *pt;

    // A slot the kernel itself uses (the identity map) is shared by every
    // address space; user pages must never be added to it.
    if ((g_pml4[slot] & PAGE_PRESENT) != 0) {
      return 0;
    }
    pdpt = user_table_next(pml4, slot);
    pd = pdpt != 0 ? user_table_next(pdpt, page_index(virt, 30)) : 0;
    pt = pd != 0 ? user_table_next(pd, page_index(virt, 21)) : 0;
    if (pt == 0) {
      return 0;
    }
    pt[page_index(virt, 12)] = (phys_start + offset) | flags;
    if (read_cr3() == cr3) {
      __asm__ volatile("invlpg (%0)" : : "r"((void *)(uintptr_t)virt) : "memory");
    }
  }
  return 1;
}

// Free a page table and everything below it (leaf pages are not freed).
static void free_table_tree(uint64_t *table, int level) {
  uint64_t i;

  if (level > 1) {
    for (i = 0; i < 512; ++i) {
      if ((table[i] & PAGE_PRESENT) != 0) {
        free_table_tree((uint64_t *)(uintptr_t)(table[i] & 0x000FFFFFFFFFF000ULL), level - 1);
      }
    }
  }
  pmm_free_page((uint64_t)(uintptr_t)table);
}

static void paging_destroy_address_space_locked(uint64_t cr3) {
  uint64_t *pml4 = (uint64_t *)(uintptr_t)cr3;
  uint64_t i;

  if (cr3 == 0 || cr3 == (uint64_t)(uintptr_t)g_pml4 || read_cr3() == cr3) {
    return;                    // never the kernel's tables, never the active ones
  }
  for (i = 0; i < 512; ++i) {
    if ((pml4[i] & PAGE_PRESENT) != 0 && pml4[i] != g_pml4[i]) {
      free_table_tree((uint64_t *)(uintptr_t)(pml4[i] & 0x000FFFFFFFFFF000ULL), 3);
    }
  }
  pmm_free_page(cr3);
}

int paging_user_range_ok(uint64_t virt, uint64_t len) {
  uint64_t addr;
  uint64_t end;

  if (g_pml4 == 0) {
    return 0;
  }
  if (len == 0) {
    return 1;
  }
  if (virt >= USER_VA_LIMIT || len > USER_VA_LIMIT - virt) {
    return 0;
  }

  addr = align_down(virt, PAGE_SIZE_BYTES);
  end = virt + len;
  while (addr < end) {
    uint64_t entry;
    uint64_t *table = (uint64_t *)(uintptr_t)read_cr3();   // the active address space
    int level;
    // Walk PML4 -> PDPT -> PD -> PT; every level must be present and user-accessible.
    for (level = 39; level >= 12; level -= 9) {
      entry = table[page_index(addr, (uint64_t)level)];
      if ((entry & PAGE_PRESENT) == 0 || (entry & PAGE_USER) == 0) {
        return 0;
      }
      if (level == 12) {
        break;
      }
      // A 1 GiB or 2 MiB leaf (PS bit) ends the walk early; this kernel only
      // creates 4 KiB mappings, so treat large pages as not user memory.
      if ((entry & 0x80ULL) != 0) {
        return 0;
      }
      table = (uint64_t *)(uintptr_t)(entry & 0x000FFFFFFFFFF000ULL);
    }
    addr += PAGE_SIZE_BYTES;
  }
  return 1;
}

static void paging_unmap_user_locked(uint64_t virt_start, uint64_t size) {
  uint64_t addr = align_down(virt_start, PAGE_SIZE_BYTES);
  uint64_t end = align_up(virt_start + size, PAGE_SIZE_BYTES);

  if (virt_start >= USER_VA_LIMIT) {
    return;
  }
  while (addr < end && addr < USER_VA_LIMIT) {
    clear_page_virt(addr);
    addr += PAGE_SIZE_BYTES;
  }
}

// ---- Locking ------------------------------------------------------------------
// The page tables (the kernel's shared lower levels, the table pool, and the PML4
// copy made for a new address space) are edited from several tasks and the timer
// tick can switch between them, so every public editing function runs with
// interrupts masked. The sections are short: one call maps at most a few hundred
// pages. irq_save/irq_restore nest, so calls from code that already masks are fine.

int paging_map_range(uint64_t phys_start, uint64_t size) {
  uint64_t flags = irq_save();
  int result = paging_map_range_locked(phys_start, size);

  irq_restore(flags);
  return result;
}

int paging_map_mmio(uint64_t phys_start, uint64_t size) {
  uint64_t flags = irq_save();
  int result = paging_map_mmio_locked(phys_start, size);

  irq_restore(flags);
  return result;
}

void paging_unmap_identity_range(uint64_t phys_start, uint64_t size) {
  uint64_t flags = irq_save();

  paging_unmap_identity_range_locked(phys_start, size);
  irq_restore(flags);
}

int paging_map_user(uint64_t virt_start, uint64_t phys_start, uint64_t size, int writable) {
  uint64_t flags = irq_save();
  int result = paging_map_user_locked(virt_start, phys_start, size, writable);

  irq_restore(flags);
  return result;
}

uint64_t paging_create_address_space(void) {
  uint64_t flags = irq_save();
  uint64_t result = paging_create_address_space_locked();

  irq_restore(flags);
  return result;
}

int paging_map_user_in(uint64_t cr3, uint64_t virt_start, uint64_t phys_start, uint64_t size, int writable) {
  uint64_t flags = irq_save();
  int result = paging_map_user_in_locked(cr3, virt_start, phys_start, size, writable);

  irq_restore(flags);
  return result;
}

void paging_destroy_address_space(uint64_t cr3) {
  uint64_t flags = irq_save();

  paging_destroy_address_space_locked(cr3);
  irq_restore(flags);
}

void paging_unmap_user(uint64_t virt_start, uint64_t size) {
  uint64_t flags = irq_save();

  paging_unmap_user_locked(virt_start, size);
  irq_restore(flags);
}

void paging_activate(uint64_t cr3) {
  uint64_t flags = irq_save();

  paging_activate_locked(cr3);
  irq_restore(flags);
}
