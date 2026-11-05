#include "heap.h"
#include "irqlock.h"
#include "pmm.h"
#include "paging.h"

#define HEAP_PAGES      1024ULL         // 4 MB of heap
#define HEAP_GUARD_PAGES 1ULL
#define HEAP_TOTAL_PAGES (HEAP_PAGES + (HEAP_GUARD_PAGES * 2ULL))
#define HEAP_ALIGN      16ULL           // allocation alignment (bytes)
#define BLOCK_MAGIC     0x4B4C42485053ULL  // "SPHBLK"

// Every allocation is preceded by this header.
typedef struct HeapBlock {
    uint64_t magic;         // sanity / corruption check
    uint64_t size;          // usable bytes after the header
    uint64_t free;          // 1 = free, 0 = in use
    struct HeapBlock *next; // next block (or 0 if last)
} HeapBlock;

static HeapBlock *g_heap_start = 0;
static uint64_t   g_heap_size  = 0;
static uint64_t   g_used_bytes = 0;

// Round up `v` to the next multiple of `align`.
static uint64_t align_up(uint64_t v, uint64_t align) {
    return (v + align - 1) & ~(align - 1);
}

void heap_init(void)
{
    uint64_t base = pmm_alloc_pages(HEAP_TOTAL_PAGES);
    if (base == 0) return;  // PMM exhausted — heap unavailable

    // PMM pages are not pre-mapped; identity-map the full heap region
    // (including guard pages) so writes below don't page-fault.
    // Guard pages will be unmapped immediately after.
    if (!paging_map_range(base, PAGE_SIZE * HEAP_TOTAL_PAGES)) return;

    g_heap_size  = HEAP_PAGES * PAGE_SIZE;
    g_heap_start = (HeapBlock *)(uintptr_t)(base + PAGE_SIZE * HEAP_GUARD_PAGES);
    g_used_bytes = 0;

    paging_unmap_identity_range(base, PAGE_SIZE * HEAP_GUARD_PAGES);
    paging_unmap_identity_range(base + ((HEAP_TOTAL_PAGES - HEAP_GUARD_PAGES) * PAGE_SIZE),
                                PAGE_SIZE * HEAP_GUARD_PAGES);

    // Single free block spanning the entire heap region.
    g_heap_start->magic = BLOCK_MAGIC;
    g_heap_start->size  = g_heap_size - sizeof(HeapBlock);
    g_heap_start->free  = 1;
    g_heap_start->next  = 0;
}

static void *kmalloc_unlocked(uint64_t size)
{
    if (g_heap_start == 0 || size == 0) return 0;

    size = align_up(size, HEAP_ALIGN);

    HeapBlock *blk = g_heap_start;
    while (blk != 0) {
        if (blk->free && blk->size >= size) {
            // Split the block if there is enough room for a new header + alignment.
            uint64_t leftover = blk->size - size;
            if (leftover > sizeof(HeapBlock) + HEAP_ALIGN) {
                HeapBlock *split = (HeapBlock *)((uint8_t *)(blk + 1) + size);
                split->magic = BLOCK_MAGIC;
                split->size  = leftover - sizeof(HeapBlock);
                split->free  = 1;
                split->next  = blk->next;

                blk->size = size;
                blk->next = split;
            }
            blk->free  = 0;
            g_used_bytes += blk->size;
            return (void *)(blk + 1);
        }
        blk = blk->next;
    }
    return 0;
}

static void kfree_unlocked(void *ptr)
{
    if (ptr == 0) return;

    HeapBlock *blk = (HeapBlock *)ptr - 1;
    if (blk->magic != BLOCK_MAGIC) return;  // invalid or corrupted pointer
    if (blk->free) return;                   // double-free guard

    blk->free = 1;
    if (g_used_bytes >= blk->size) g_used_bytes -= blk->size;

    // Coalesce with the physically adjacent next block if it is also free.
    if (blk->next != 0 && blk->next->free) {
        blk->size += sizeof(HeapBlock) + blk->next->size;
        blk->next  = blk->next->next;
    }
}

uint64_t heap_used_bytes(void)
{
    return g_used_bytes;
}

// Allocation can be preempted by the scheduler tick, so keep each call atomic.
void *kmalloc(uint64_t size) {
  uint64_t flags = irq_save();
  void *result = kmalloc_unlocked(size);
  irq_restore(flags);
  return result;
}

void kfree(void *ptr) {
  uint64_t flags = irq_save();
  kfree_unlocked(ptr);
  irq_restore(flags);
}
