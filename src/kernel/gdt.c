#include "console.h"
#include "gdt.h"

// Minimal 3-entry GDT in .data so the values are baked in at compile time.
// Being in .data (not .bss) means they are non-zero and in the kernel binary,
// but more importantly they live inside the kernel image region that
// paging_init() identity-maps, so the CPU can read them after CR3 is loaded.
//
// Descriptor format (little-endian 64-bit):
//   [15:0]  Limit 15:0
//   [39:16] Base  23:0
//   [47:40] Access byte
//   [51:48] Limit 19:16
//   [55:52] Flags
//   [63:56] Base  31:24
//
// Kernel 64-bit code (index 1, selector 0x08):
//   L=1, G=1, P=1, S=1, DPL=0, type=code/exec/read  →  0x00AF9A000000FFFF
// Kernel 64-bit data (index 2, selector 0x10):
//   G=1, D/B=1, P=1, S=1, DPL=0, type=data/read/write →  0x00CF92000000FFFF
// User 64-bit code (index 3, selector 0x2B = 3*8 + 3):
//   L=1, G=1, P=1, S=1, DPL=3, type=code/exec/read  →  0x00AFFA000000FFFF
// User 64-bit data (index 4, selector 0x33 = 4*8 + 3):
//   G=1, D/B=1, P=1, S=1, DPL=3, type=data/read/write →  0x00CFF2000000FFFF
static uint64_t g_gdt[5] = {
    0x0000000000000000ULL,
    0x00AF9A000000FFFFULL,
    0x00CF92000000FFFFULL,
    0x00AFFA000000FFFFULL,
    0x00CFF2000000FFFFULL,
};

typedef struct {
    unsigned short Limit;
    uint64_t Base;
} __attribute__((packed)) GDTPointer;

static GDTPointer g_gdt_ptr;

unsigned short gdt_init(void) {
    g_gdt_ptr.Limit = (unsigned short)(sizeof(g_gdt) - 1U);
    g_gdt_ptr.Base  = (uint64_t)(uintptr_t)g_gdt;

    // Load the new GDT and perform a far return to reload CS with selector 0x08.
    // The leaq-based label approach works in PIE code (RIP-relative).
    __asm__ volatile(
        "lgdt %0\n\t"
        "pushq $0x08\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "movw $0x10, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "xorw %%ax, %%ax\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        :
        : "m"(g_gdt_ptr)
        : "rax", "memory"
    );

    return 0x08U;
}
