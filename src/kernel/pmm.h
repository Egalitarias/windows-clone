#ifndef KERNEL_PMM_H
#define KERNEL_PMM_H

#include "console.h"  // for uint*_t typedefs

#define PAGE_SIZE       4096ULL
#define PMM_MAX_PAGES   262144ULL   // covers 0..1 GB of physical RAM

// EFI memory descriptor (from UEFI spec 2.x)
#define EFI_MEMORY_TYPE_CONVENTIONAL 7U

typedef struct {
    uint32_t Type;
    uint32_t Pad;
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

// Initialise the PMM from the UEFI memory map.
// memory_map   : pointer to the first EFI_MEMORY_DESCRIPTOR
// map_size     : total bytes of the map
// desc_size    : bytes per descriptor (use this, NOT sizeof, for portability)
// entries      : number of descriptors
void pmm_init(const void *memory_map, uint64_t map_size,
              uint64_t desc_size, uint64_t entries);

// Allocate a single 4 KB page. Returns the physical address, or 0 on failure.
uint64_t pmm_alloc_page(void);

// Allocate `n` physically contiguous 4 KB pages.
// Returns the physical address of the first page, or 0 on failure.
uint64_t pmm_alloc_pages(uint64_t n);

// Return a previously allocated page to the free pool.
void pmm_free_page(uint64_t phys_addr);

// Number of free pages currently available.
uint64_t pmm_available_pages(void);

#endif // KERNEL_PMM_H
