#include "console.h"
#include "paging.h"

#define PAGE_SIZE_BYTES 4096ULL
#define PAGE_PRESENT    0x001ULL
#define PAGE_WRITE      0x002ULL
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

static uint64_t *ensure_table(uint64_t *parent, uint64_t index) {
  uint64_t entry;
  uint64_t *child;

  entry = parent[index];
  if ((entry & PAGE_PRESENT) != 0) {
    return (uint64_t *)(uintptr_t)(entry & 0x000FFFFFFFFFF000ULL);
  }

  child = alloc_page_table();
  if (child == 0) {
    return 0;
  }

  parent[index] = ((uint64_t)(uintptr_t)child) | PAGE_PRESENT | PAGE_WRITE;
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

  if (g_pml4 == 0) {
    return 0;
  }

  pml4_index = page_index(virt, 39);
  pdpt_index = page_index(virt, 30);
  pd_index = page_index(virt, 21);
  pt_index = page_index(virt, 12);

  pdpt = ensure_table(g_pml4, pml4_index);
  if (pdpt == 0) {
    return 0;
  }

  pd = ensure_table(pdpt, pdpt_index);
  if (pd == 0) {
    return 0;
  }

  pt = ensure_table(pd, pd_index);
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

int paging_map_range(uint64_t phys_start, uint64_t size) {
  return map_range_virt(phys_start, phys_start, size, PAGE_WRITE);
}

void paging_unmap_identity_range(uint64_t phys_start, uint64_t size) {
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
