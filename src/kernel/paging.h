#ifndef KERNEL_PAGING_H
#define KERNEL_PAGING_H

#include "pmm.h"

#define KERNEL_HIGH_HALF_BASE 0xFFFF800000000000ULL

int paging_init(const EFI_MEMORY_DESCRIPTOR *memory_map,
                uint64_t kernel_image_base,
                uint64_t kernel_image_size,
                uint64_t desc_size,
                uint64_t entries);

void paging_set_page_table_pool(void *pool, uint64_t size);

// Identity-map a physical range in the active page tables (writable, no-exec).
// Safe to call after paging_init has activated CR3.
int paging_map_range(uint64_t phys_start, uint64_t size);

// Identity-map device memory (MMIO) as uncached. Use this for PCI BARs; a
// cacheable mapping would let the CPU cache or reorder register accesses.
int paging_map_mmio(uint64_t phys_start, uint64_t size);

// Map [virt, virt+size) to physical memory at phys with the U/S bit set so ring 3
// can access it. Both addresses must be page aligned and virt must be in the
// lower (user) half. The caller must pick a virtual range that nothing else maps.
int paging_map_user(uint64_t virt_start, uint64_t phys_start, uint64_t size, int writable);

void paging_unmap_identity_range(uint64_t phys_start, uint64_t size);

#endif // KERNEL_PAGING_H
