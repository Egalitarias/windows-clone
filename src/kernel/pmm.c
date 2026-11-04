#include "pmm.h"

// One bit per 4 KB page frame.  0 = free, 1 = used.
// 32 KB covers up to 1 GB of physical address space.
// Keep the bitmap 16-byte aligned so compiler-generated SIMD stores are safe.
static uint8_t pmm_bitmap[PMM_MAX_PAGES / 8] __attribute__((aligned(16)));

static uint64_t pmm_free_count = 0;

// ---- bitmap helpers -------------------------------------------------------

static void pmm_set_used(uint64_t page) {
    if (page >= PMM_MAX_PAGES) return;
    pmm_bitmap[page / 8] |= (uint8_t)(1u << (page % 8));
}

static void pmm_set_free(uint64_t page) {
    if (page >= PMM_MAX_PAGES) return;
    pmm_bitmap[page / 8] &= (uint8_t)(~(1u << (page % 8)));
}

static int pmm_is_used(uint64_t page) {
    if (page >= PMM_MAX_PAGES) return 1;
    return (pmm_bitmap[page / 8] >> (page % 8)) & 1u;
}

// ---- public API -----------------------------------------------------------

void pmm_init(const void *memory_map, uint64_t map_size,
              uint64_t desc_size, uint64_t entries)
{
    uint64_t i;

    // Start with everything marked as used.
    for (i = 0; i < sizeof(pmm_bitmap); ++i) {
        ((volatile uint8_t *)pmm_bitmap)[i] = 0xFF;
    }
    pmm_free_count = 0;

    if (memory_map == 0 || desc_size == 0 || entries == 0) return;
    (void)map_size;

    for (i = 0; i < entries; ++i) {
        const EFI_MEMORY_DESCRIPTOR *desc =
            (const EFI_MEMORY_DESCRIPTOR *)
            ((const uint8_t *)memory_map + i * desc_size);

        if (desc->Type != EFI_MEMORY_TYPE_CONVENTIONAL) continue;

        uint64_t base  = desc->PhysicalStart;
        uint64_t pages = desc->NumberOfPages;
        uint64_t p;

        for (p = 0; p < pages; ++p) {
            uint64_t page_frame = (base / PAGE_SIZE) + p;
            if (page_frame == 0) continue;          // keep null page reserved
            if (page_frame >= PMM_MAX_PAGES) break;  // beyond bitmap coverage
            pmm_set_free(page_frame);
            ++pmm_free_count;
        }
    }
}

uint64_t pmm_alloc_page(void)
{
    uint64_t i;
    for (i = 1; i < PMM_MAX_PAGES; ++i) {
        if (!pmm_is_used(i)) {
            pmm_set_used(i);
            if (pmm_free_count > 0) --pmm_free_count;
            return i * PAGE_SIZE;
        }
    }
    return 0;
}

uint64_t pmm_alloc_pages(uint64_t n)
{
    uint64_t start = 1;
    uint64_t run   = 0;
    uint64_t i;

    if (n == 0) return 0;

    for (i = 1; i < PMM_MAX_PAGES; ++i) {
        if (!pmm_is_used(i)) {
            if (run == 0) start = i;
            ++run;
            if (run == n) {
                uint64_t p;
                for (p = start; p < start + n; ++p) {
                    pmm_set_used(p);
                }
                if (pmm_free_count >= n) pmm_free_count -= n;
                else pmm_free_count = 0;
                return start * PAGE_SIZE;
            }
        } else {
            run = 0;
        }
    }
    return 0;
}

void pmm_free_page(uint64_t phys_addr)
{
    uint64_t page_frame = phys_addr / PAGE_SIZE;
    if (page_frame == 0 || page_frame >= PMM_MAX_PAGES) return;
    if (pmm_is_used(page_frame)) {
        pmm_set_free(page_frame);
        ++pmm_free_count;
    }
}

uint64_t pmm_available_pages(void)
{
    return pmm_free_count;
}
