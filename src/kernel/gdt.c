#include "console.h"
#include "gdt.h"

// Kernel GDT in .data so the values are baked in at compile time.
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
// Kernel 64-bit code (index 1, selector 0x08):  0x00AF9A000000FFFF
// Kernel 64-bit data (index 2, selector 0x10):  0x00CF92000000FFFF
// User   64-bit data (index 3, selector 0x1B):  0x00CFF2000000FFFF  (DPL=3)
// User   64-bit code (index 4, selector 0x23):  0x00AFFA000000FFFF  (DPL=3)
// TSS    (index 5-6, selector 0x28): 16-byte system descriptor, filled in at
//        runtime by gdt_init() because it holds the TSS address.
static uint64_t g_gdt[7] = {
    0x0000000000000000ULL,
    0x00AF9A000000FFFFULL,
    0x00CF92000000FFFFULL,
    0x00CFF2000000FFFFULL,
    0x00AFFA000000FFFFULL,
    0x0000000000000000ULL,
    0x0000000000000000ULL,
};

// 64-bit Task State Segment. Only rsp0 matters for us: it is the stack the CPU
// loads on a ring 3 -> ring 0 transition via an interrupt or exception.
typedef struct {
    uint32_t Reserved0;
    uint64_t Rsp[3];
    uint64_t Reserved1;
    uint64_t Ist[7];
    uint64_t Reserved2;
    uint16_t Reserved3;
    uint16_t IoMapBase;
} __attribute__((packed)) TSS64;

static TSS64 g_tss;

typedef struct {
    unsigned short Limit;
    uint64_t Base;
} __attribute__((packed)) GDTPointer;

static GDTPointer g_gdt_ptr;

static void gdt_install_tss(void) {
    uint64_t base = (uint64_t)(uintptr_t)&g_tss;
    uint64_t limit = (uint64_t)sizeof(g_tss) - 1ULL;

    g_tss.IoMapBase = (uint16_t)sizeof(g_tss);  // no I/O permission bitmap

    // Access 0x89 = present, DPL=0, type 9 (available 64-bit TSS).
    g_gdt[5] = (limit & 0xFFFFULL)
             | ((base & 0xFFFFFFULL) << 16)
             | (0x89ULL << 40)
             | (((limit >> 16) & 0xFULL) << 48)
             | (((base >> 24) & 0xFFULL) << 56);
    g_gdt[6] = base >> 32;
}

unsigned short gdt_init(void) {
    gdt_install_tss();

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

    // Load the task register. The CPU marks the descriptor busy, so ltr must
    // only run once per descriptor.
    __asm__ volatile("ltr %0" : : "r"((uint16_t)GDT_TSS) : "memory");

    return 0x08U;
}

void gdt_set_kernel_stack(uint64_t rsp0) {
    g_tss.Rsp[0] = rsp0;
}

uint64_t gdt_get_kernel_stack(void) {
    return g_tss.Rsp[0];
}
