# User-Space Memory Layout Design

## Overview

This document describes the virtual address space layout for user-mode processes in the secure personal OS. Each user process gets its own isolated 64-bit virtual address space, with kernel code and data mapped read-only (or not accessible) into each user's page tables.

## Memory Space Partitioning

The 64-bit virtual address space is divided into user-accessible and kernel-only regions:

```
Virtual Address Space Layout (64-bit x86-64)

0xFFFFFFFF_FFFFFFFF  ┌─────────────────────┐
                     │  Canonical Sign Ext │ (invalid addresses)
                     │  (upper 16 bits)    │
0xFFFF8000_00000000  ├─────────────────────┤
                     │  KERNEL VA SPACE    │ ← Kernel-only (paging enforces)
                     │  (2 TB, fixed)      │   Shared across all processes
                     │  0xFFFF800000000000 │   - Kernel code (read-only)
                     │  0xFFFFFFFFFFFFFFFF │   - Kernel data
                     │  (256 PML4 entries) │   - Direct physical map
                     │                     │   - Heap, stacks
0x00007FFF_FFFFFFFF  ├─────────────────────┤
                     │  USER VA SPACE      │ ← User-accessible per-process
                     │  (~128 TB max)      │   Private mappings:
                     │  0x00000000_00000000│   - Code (.text)
                     │  0x00007FFF_FFFFFFFF│   - Data (.data, .bss)
                     │  (256 PML4 entries) │   - Heap (grows up)
                     │  minus reserved     │   - Stack (grows down)
                     │                     │
0x00000000_00000000  └─────────────────────┘
```

## User Process Address Space

### Standard Layout

```
User VA Space (Per-Process)

0x7FFFFFFFFFFF  ┌──────────────────────────┐
                │  (Reserved/Unused)       │
                │                          │
0x7FFFFFFDE000  ├──────────────────────────┤
                │  STACK (guard-protected) │ Grows DOWN (towards 0x0)
                │  Base: 0x7FFFFFFDE000    │ Top:  0x7FFFFFFFFFFF
                │  Size: 8 KB typical      │ Stack pointer decreases
                │  Guard pages above/below │ Overflow detection
                │  Watermark checks        │
0x7FFFFFFDD000  ├──────────────────────────┤
                │  (Unused Gap)            │
                │  Collision buffer        │
                │  ~0x800000 bytes         │
0x7FFFE00000    ├──────────────────────────┤
                │  (Reserved for future)   │
                │                          │
0x01000000      ├──────────────────────────┤
                │  HEAP                    │ Grows UP (towards stack)
                │  Base: 0x01000000        │ Collision detection:
                │  (Variable size)         │ heap + stack > limit → FAIL
                │  Grows up on malloc()    │
0x00800000      ├──────────────────────────┤
                │  (Unused buffer)         │ 0x800000 bytes (8 MB)
                │  Collision detection    │ Prevents accidental overlap
0x00000000      ├──────────────────────────┤
                │  RESERVED (NULL ptr)     │ Not mapped
                │  Null dereference trap   │ Accessing causes page fault
                │  1 page (4 KB)           │ Debuggable segfault
0x00000000      └──────────────────────────┘
```

### Address Ranges

| Region | Start | End | Size | Purpose | Grows |
|--------|-------|-----|------|---------|-------|
| Reserved NULL | 0x0 | 0x1000 | 4 KB | Trap null deref | - |
| (Buffer) | 0x1000 | 0x800000 | ~8 MB | Collision buffer | - |
| Heap | 0x800000 | Dynamic | Variable | Dynamic memory | Up |
| (Gap) | Dynamic | 0x7FFFFFFDD000 | Varies | Free space | - |
| Stack Guard (low) | 0x7FFFFFFDD000 | 0x7FFFFFFDE000 | 4 KB | Guard page | - |
| Stack (usable) | 0x7FFFFFFDE000 | 0x7FFFFFFFE000 | 8 KB | Call stack | Down |
| Stack Guard (high) | 0x7FFFFFFFE000 | 0x7FFFFFFFF000 | 4 KB | Guard page | - |

## Implementation Details

### NULL Pointer Detection

The first 4 KB (0x0000000000000000 - 0x0000000000001000) is intentionally unmapped:

```c
// Not mapped in user page tables
// Accessing this region triggers page fault (Vector 14)
// Kernel fault handler reports "Segmentation Fault" to user process

void *p = NULL;
*p = 0;  // → Page Fault Vector 14 → Process terminated
```

**Benefit:** Catches null pointer dereferences at hardware level (fast, deterministic)

### Stack Layout

The stack is allocated as an 8 KB page with 4 KB guard pages above and below (16 KB total):

```
User Stack Layout

0x7FFFFFFFFFFF  ┌─────────────────────┐
                │ (Inaccessible)      │ 
0x7FFFFFFFF000  │ Guard Page (high)   │ Not mapped, page fault if accessed
0x7FFFFFFFE000  ├─────────────────────┤
                │ (RSP when empty)    │ Stack grows DOWN (decreases)
                │ (valid stack space) │
                │ Local variables     │
                │ Return addresses    │
0x7FFFFFFDE000  ├─────────────────────┤
                │ Guard Page (low)    │ Not mapped, page fault if accessed
0x7FFFFFFDD000  │ (Inaccessible)      │
                └─────────────────────┘
```

**Guard Pages:**
- Located above and below the actual stack page
- Not mapped (page fault on access)
- Detects stack overflow: accessing guard page → fault → can terminate safely
- Detects buffer overflow: canary corruption → guard page touched → detected

**Watermark Checking:**
```c
#define STACK_GUARD_WATERMARK_LOW  0xDEADBEEFCAFEBABE
#define STACK_GUARD_WATERMARK_HIGH 0xDEADBEEFCAFEBABE

// Written at stack boundaries during allocation
uint64_t *stack_low = (uint64_t *)stack_base;
uint64_t *stack_high = (uint64_t *)(stack_base + stack_size - 8);

*stack_low = STACK_GUARD_WATERMARK_LOW;
*stack_high = STACK_GUARD_WATERMARK_HIGH;

// Checked periodically to detect buffer overflows
assert(*stack_low == STACK_GUARD_WATERMARK_LOW);
assert(*stack_high == STACK_GUARD_WATERMARK_HIGH);
```

### Heap Layout

Heap is allocated dynamically starting at 0x800000:

```
Heap Growth

malloc() #1 → 0x800000 (initial chunk, e.g., 64 KB)
malloc() #2 → 0x810000 (next chunk, e.g., 64 KB)
...
malloc() #N → grows towards stack limit check
```

**Heap Management:**
- Simple bump allocator or linked list allocator
- Collision detection: ensure `heap_top + heap_size < stack_base - BUFFER`
- Out of memory: heap growth fails, process gets ENOMEM

### Collision Detection

```c
#define USER_STACK_BASE     0x7FFFFFFDE000
#define USER_HEAP_START     0x00800000
#define COLLISION_BUFFER    0x00800000  // 8 MB minimum gap

// After each malloc()
if (heap_top + new_size > USER_STACK_BASE - COLLISION_BUFFER) {
    // Heap would collide with stack
    return NULL;  // malloc failure
}
```

**Why 8 MB buffer?**
- Provides safety margin for both stack and heap growth
- Prevents edge-case collisions in heavily loaded processes
- Allows detection before actual fault occurs

## Kernel VA Space (Shared)

Every user process's page tables include the kernel VA space:

```
Kernel VA Space (Shared Mapping)

0xFFFFFFFFFFFFFFFF  ┌──────────────────────┐
                    │  (Reserved/Unused)   │
                    │                      │
0xFFFF828000000000  ├──────────────────────┤
                    │  Direct Physical Map │ Maps all physical RAM
                    │  0xFFFF800000000000  │ VA = 0xFFFF800000000000
                    │                      │ PA = 0x0 (identity)
                    │  Kernel heaps        │
                    │  Kernel stacks       │
0xFFFF800000000000  ├──────────────────────┤
                    │  Kernel Code/Data    │ Read-only for users
                    │  (High half kernel)  │ Execute-only for code
                    │  0xFFFF800000000000  │ DPL=0, SUP=1
                    │  Paging enforces     │ User can't write/execute
                    │  access restrictions│ without privilege escalation
                    └──────────────────────┘
```

**Key Properties:**
- Kernel VA space at 0xFFFF800000000000+ is identity-mapped to physical RAM
- Every user process's CR3 (page table) includes kernel mappings
- Mapped as read-only (W=0) for user-mode code (DPL=3)
- Supervisor-only mappings (SUP=1) prevent user access

## Process Initialization

### Stack Setup for User Entry Point

When a user process is created, its stack is initialized with argc/argv:

```c
// Top of stack before first instruction
struct {
    uint64_t argc;                      // Argument count
    uint64_t *argv;                     // Pointer to arg pointers
    uint64_t *envp;                     // Pointer to env vars
    // (possibly return address if called as function)
} *stack_base;

// Process entry: user code receives:
// RDI = argc
// RSI = argv[] pointer
// RDX = envp[] pointer
// RSP = points to return address (or lower on stack)
```

### Context Switch to User Code

When switching from kernel to user process:

```asm
// Prepare for SYSRET or IRETQ to user code:
movq  $GDT_USER_SS, %rax      // User data segment selector
movq  %rax, %ss               // Load user SS
movq  user_rsp, %rsp          // Load user stack pointer

pushq $GDT_USER_CS            // Push user code selector
pushq $user_entry_point       // Push user code address
pushq $user_rflags            // Push user RFLAGS
// ... load user general purpose registers ...

// Jump to user code with privilege drop:
lretq                         // Loads CS from stack, drops privilege

// OR using faster SYSRET:
mov   $user_entry_point, %rcx // RCX = user code address
sysretq                       // Atomically switches to Ring 3
```

## Protection Mechanisms

### 1. Privilege Level Enforcement (CPU)

```
Ring 3 (User) code cannot:
- Execute Ring 0-only instructions (CLI, STI, LGDT, etc.)
- Access Ring 0 (kernel) memory (SUP=1 pages)
- Modify CR registers (CR0, CR3, CR4, etc.)
- Access MSRs (Model-Specific Registers)

Violations → General Protection Fault (Vector 13)
```

### 2. Paging (Virtual Memory)

```
User page table permissions:
- User stack: RW (read-write, user-accessible)
- User heap: RW (read-write, user-accessible)
- User code: R+X (read+execute, user-accessible)
- Kernel mappings: supervisory (SUP=1, user cannot access)

Violations → Page Fault (Vector 14)
```

### 3. Guard Pages (Process-Level)

```
Stack overflow → Page Fault → Fault handler detects guard page
                            → Terminates process gracefully
                            → No system corruption
```

### 4. Watermark Checks (Debugging)

```
Periodic checks on watermark values can detect:
- Stack buffer overflows
- Use-after-free
- Heap corruption
```

## Address Space Isolation

### Per-Process vs. Kernel Visibility

Each process sees a different virtual address space:

```
Process A:
  0x00000000: Process A code
  0x00800000: Process A heap
  0x7FFFFFFDE000: Process A stack

Process B:
  0x00000000: Process B code (completely separate!)
  0x00800000: Process B heap (independent allocation)
  0x7FFFFFFDE000: Process B stack (isolated)

Both processes see:
  0xFFFF800000000000+: Kernel code/data (shared, read-only)
```

**Implementation:**
- Each process has its own PML4 page table (CR3 base)
- Kernel mappings copied into each PML4 during address space creation
- Context switch loads CR3 with process's PML4
- Paging hardware translates VA → PA using process-specific tables

## Design Decisions

### Q: Why 0x800000 for heap base?

**A:** 8 MB reserved for NULL pointer guard and safety margin.
- 0x000000-0x800000 = 8 MB buffer
- Catches most null pointer bugs (common case)
- Prevents accidental NULL access
- Still leaves plenty of heap space (up to ~120 TB)

### Q: Why 0x7FFFFFFDE000 for stack base?

**A:** Maximum user VA (0x7FFFFFFFFFFF) with safety margin below.
- 0x7FFFFFFFF000 is highest 4 KB page (guard page high)
- 0x7FFFFFFFE000 is actual stack page
- 0x7FFFFFFDE000 is stack base address
- Leaves 2 MB guard above for safety

### Q: Why shared kernel mappings in each user CR3?

**A:** Simplifies syscall/context switch efficiency.
- No page table switching overhead for syscalls
- Kernel can directly access user stacks (for signal delivery, debugging)
- Paging (SUP=1) prevents user from accessing kernel memory
- Alternative: separate kernel/user CR3 (more complex)

### Q: Why separate heap and stack?

**A:** Collision detection and protection.
- Detectable grow-together condition
- Guard regions prevent silent data corruption
- Can log heap/stack exhaustion events
- Alternative: combined arena (less safe)

## Testing

Test cases verify:
1. Null pointer access → Page Fault
2. Stack overflow (write guard page) → Page Fault
3. Heap-stack collision detection → ENOMEM
4. Address space isolation between processes
5. Kernel mappings accessible but read-only

## Future Enhancements

1. **ASLR (Address Space Layout Randomization)**
   - Randomize heap/stack bases per process
   - Thwarts address-guessing attacks
   - Requires relocation tables

2. **DEP/NX (Data Execution Prevention)**
   - Mark data pages non-executable
   - Thwarts code injection attacks
   - Requires page table NX bit support

3. **Secure Enclave Isolation**
   - Separate VA space for sensitive operations
   - Hardware TEE (Trusted Execution Environment)
   - Future: Intel SGX / AMD SEV

## References

- x86-64 System V ABI: Memory Layout conventions
- Linux kernel: mm/mmap.c (memory allocation)
- macOS ASLR: https://support.apple.com/en-us/HT6228
- Intel SDM Vol. 3A: Paging and virtual address translation

---

**Document Version:** 1.0
**Last Updated:** 2026-07-08
**Status:** Design Complete - Phase 1 Foundation
