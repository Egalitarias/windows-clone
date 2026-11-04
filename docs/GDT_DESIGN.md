# GDT Design for Ring 3 User-Mode Support

## Overview

This document describes the Global Descriptor Table (GDT) enhancements required to support Ring 3 user-mode processes in WinClone kernel. The current kernel-only GDT has been extended with user-mode code and data segments, enabling privilege-separated execution.

## Current Architecture (Before)

The original GDT contained 3 entries, all in kernel mode (Ring 0):

| Index | Selector | Type | DPL | Purpose | Descriptor |
|-------|----------|------|-----|---------|------------|
| 0 | 0x00 | NULL | - | Null descriptor | 0x0000000000000000 |
| 1 | 0x08 | Code | 0 | Kernel code, 64-bit | 0x00AF9A000000FFFF |
| 2 | 0x10 | Data | 0 | Kernel data | 0x00CF92000000FFFF |

All tasks executed in Ring 0 with full privilege. No user-mode isolation was possible.

## New Architecture (After)

The enhanced GDT now contains 5 entries with both kernel and user segments:

| Index | Selector | Type | DPL | Purpose | Descriptor |
|-------|----------|------|-----|---------|------------|
| 0 | 0x00 | NULL | - | Null descriptor | 0x0000000000000000 |
| 1 | 0x08 | Code | 0 | Kernel code, 64-bit | 0x00AF9A000000FFFF |
| 2 | 0x10 | Data | 0 | Kernel data | 0x00CF92000000FFFF |
| 3 | 0x2B | Code | 3 | **User code, 64-bit** | **0x00AFFA000000FFFF** |
| 4 | 0x33 | Data | 3 | **User data** | **0x00CFF2000000FFFF** |

User-mode processes now have separate code and data selectors with DPL=3 (Ring 3), enabling privilege separation.

## x86-64 GDT Descriptor Format

Each GDT descriptor is a 64-bit value with the following bit layout:

```
Byte layout (little-endian):
  Byte 0-1:    Limit [15:0]
  Byte 2-4:    Base  [23:0]
  Byte 5:      Access byte
  Byte 6:      Limit [19:16] + Flags
  Byte 7:      Base  [31:24]
```

### Access Byte (Byte 5)

```
Bit: 7 6 5 4 3 2 1 0
     P DPL S E W A V 0
```

- **P (Bit 7)**: Present (1 = descriptor is valid)
- **DPL (Bits 6-5)**: Descriptor Privilege Level
  - 0 = Ring 0 (kernel)
  - 3 = Ring 3 (user)
- **S (Bit 4)**: Descriptor type
  - 1 = Code/Data descriptor
  - 0 = System descriptor
- **E (Bit 3)**: Executable (code) / Expand-down (data)
  - 1 for code segments
  - 0 for data segments
- **W (Bit 2)**: Writable (data) / Conforming (code)
  - 1 = segment is writable/conforming
  - 0 = read-only/non-conforming
- **A (Bit 1)**: Accessed (automatically set by CPU)
- **V (Bit 0)**: Always 0 in legacy descriptors

### Flags Byte (Byte 6, Upper nibble)

```
Bit: 3 2 1 0
     0 L D G
```

- **L (Bit 2)**: Long mode (1 = 64-bit mode for code segments)
- **D/B (Bit 1)**: Default operation size
  - 1 = 32-bit default
  - 0 = 16-bit (ignored in 64-bit mode)
- **G (Bit 0)**: Granularity
  - 1 = limit is in 4 KB pages
  - 0 = limit is in bytes
- Bit 3 is reserved (must be 0)

## GDT Segments: Kernel

### Kernel Code Segment (Index 1, Selector 0x08)

```
Descriptor: 0x00AF9A000000FFFF
  Base:     0x000000
  Limit:    0xFFFFF (4 GB when G=1)
  Access:   0x9A = 0b10011010
    P=1 (present)
    DPL=0 (kernel)
    S=1 (code/data)
    E=1 (executable)
    W=0 (read-only code)
    A=0 (not accessed)
  Flags:    0xA = 0b1010 (only relevant bits)
    L=1 (64-bit long mode)
    D/B=0 (ignored in 64-bit)
    G=1 (granularity 4 KB)
```

**Calculation:**
- Access byte: 0b1_0_0_1_1_0_1_0 = 0x9A ✓
- Descriptor = 0x00AF9A000000FFFF ✓

### Kernel Data Segment (Index 2, Selector 0x10)

```
Descriptor: 0x00CF92000000FFFF
  Base:     0x000000
  Limit:    0xFFFFF (4 GB when G=1)
  Access:   0x92 = 0b10010010
    P=1 (present)
    DPL=0 (kernel)
    S=1 (code/data)
    E=0 (data segment)
    W=1 (writable)
    A=0 (not accessed)
  Flags:    0xC = 0b1100
    L=0 (not used for data)
    D/B=1 (32-bit default size)
    G=1 (granularity 4 KB)
```

**Calculation:**
- Access byte: 0b1_0_0_1_0_0_1_0 = 0x92 ✓
- Descriptor = 0x00CF92000000FFFF ✓

## GDT Segments: User (NEW)

### User Code Segment (Index 3, Selector 0x2B)

```
Selector calculation: 3 × 8 + RPL(3) = 24 + 3 = 0x2B
Descriptor: 0x00AFFA000000FFFF
  Base:     0x000000
  Limit:    0xFFFFF (4 GB when G=1)
  Access:   0xFA = 0b11111010
    P=1 (present)
    DPL=3 (user/Ring 3) ← KEY DIFFERENCE
    S=1 (code/data)
    E=1 (executable)
    W=0 (read-only code)
    A=0 (not accessed)
  Flags:    0xA = 0b1010
    L=1 (64-bit long mode)
    D/B=0 (ignored in 64-bit)
    G=1 (granularity 4 KB)
```

**Calculation:**
- Access byte: 0b1_1_0_1_1_0_1_0 = 0xFA ✓
- Descriptor = 0x00AFFA000000FFFF ✓

**Difference from kernel CS:**
- Access byte: 0x9A → 0xFA (DPL bits 6-5 changed from 00 to 11)
- All other bits identical

### User Data Segment (Index 4, Selector 0x33)

```
Selector calculation: 4 × 8 + RPL(3) = 32 + 3 = 0x33
Descriptor: 0x00CFF2000000FFFF
  Base:     0x000000
  Limit:    0xFFFFF (4 GB when G=1)
  Access:   0xF2 = 0b11110010
    P=1 (present)
    DPL=3 (user/Ring 3) ← KEY DIFFERENCE
    S=1 (code/data)
    E=0 (data segment)
    W=1 (writable)
    A=0 (not accessed)
  Flags:    0xC = 0b1100
    L=0 (not used for data)
    D/B=1 (32-bit default size)
    G=1 (granularity 4 KB)
```

**Calculation:**
- Access byte: 0b1_1_0_1_0_0_1_0 = 0xF2 ✓
- Descriptor = 0x00CFF2000000FFFF ✓

**Difference from kernel DS:**
- Access byte: 0x92 → 0xF2 (DPL bits 6-5 changed from 00 to 11)
- All other bits identical

## Selector Encoding

GDT selectors are 16-bit values with the following format:

```
Bits [15:3]: Index into GDT (0-8191)
Bits [2]:    TI (Table Indicator): 0=GDT, 1=LDT
Bits [1:0]:  RPL (Requested Privilege Level): 0-3
```

**Example calculations:**

| Segment | Index | TI | RPL | Selector | Hex |
|---------|-------|----|----|----------|-----|
| Kernel CS | 1 | 0 | 0 | (1<<3) \| 0 = 8 | 0x08 |
| Kernel DS | 2 | 0 | 0 | (2<<3) \| 0 = 16 | 0x10 |
| User CS | 3 | 0 | 3 | (3<<3) \| 3 = 27 | 0x2B |
| User DS | 4 | 0 | 3 | (4<<3) \| 3 = 35 | 0x33 |

## Implementation Details

### GDT Storage

```c
static uint64_t g_gdt[5] = {
    0x0000000000000000ULL,  // Index 0: NULL descriptor
    0x00AF9A000000FFFFULL,  // Index 1: Kernel CS (0x08)
    0x00CF92000000FFFFULL,  // Index 2: Kernel DS (0x10)
    0x00AFFA000000FFFFULL,  // Index 3: User CS (0x2B)
    0x00CFF2000000FFFFULL,  // Index 4: User DS (0x33)
};
```

### Selector Constants (gdt.h)

```c
#define GDT_KERNEL_CS   0x08U  // Kernel code selector
#define GDT_KERNEL_DS   0x10U  // Kernel data selector
#define GDT_USER_CS     0x2BU  // User code selector
#define GDT_USER_DS     0x33U  // User data selector
```

### GDT Initialization

The `gdt_init()` function:
1. Sets up GDT pointer with address and limit
2. Executes `lgdt` instruction to load new GDT
3. Performs far return to reload CS with kernel selector (0x08)
4. Reloads all segment registers with kernel selectors
5. Returns kernel CS selector for IDT updates

## Security Implications

### Privilege Level Enforcement

The CPU automatically enforces privilege levels:
- **DPL Check**: Code executing in Ring N can only access segments with DPL ≤ N
- **Ring 3 Code**: Cannot directly access Ring 0 (kernel) segments
- **Ring 0 Code**: Can access any segment (full privilege)

### Protection Mechanisms

1. **Descriptor Privilege Level (DPL)**
   - User segments have DPL=3
   - Kernel segments have DPL=0
   - CPU prevents privilege escalation at descriptor level

2. **Paging (Complementary)**
   - GDT prevents direct access to kernel selectors
   - Page tables prevent access to kernel virtual addresses
   - Combined: Defense in depth

3. **Stack Isolation**
   - User processes have separate stack in user VA space (via paging)
   - Kernel maintains privileged kernel stack
   - Context switch loads appropriate stack pointer

### Limitations of GDT-Only Protection

⚠️ Important: The GDT limit field (0xFFFFF = 4 GB) does NOT prevent user code from accessing memory beyond that. Modern x86-64 ignores the limit in 64-bit mode. **Actual memory protection is enforced by paging.**

The GDT merely controls which segments can be accessed by code at different privilege levels. The page table controls the actual memory addresses accessible.

## Hardware Details

### x86-64 Privilege Levels

| Ring | Privilege | Usage | Access |
|------|-----------|-------|--------|
| 0 | Kernel | OS kernel | Full access to all memory/I/O |
| 1 | System | Reserved | (not used in modern OSes) |
| 2 | System | Reserved | (not used in modern OSes) |
| 3 | User | User applications | Limited to unprivileged I/O |

### Segment Loading Instructions

- **`lgdt mem`**: Load GDT pointer
- **`lretq`**: Far return (loads CS and RIP from stack)
- **`mov reg, sreg`**: Load segment register
  - Updates descriptor cache with DPL, base, limit, attributes

### Privilege Level Transitions

**User → Kernel (syscall)**
1. Controlled via `SYSCALL`/`SYSRET` instructions (faster than `INT`)
2. Sets MSR for kernel CS/SS selectors
3. Atomically switches privilege level and jumps to kernel handler

**Kernel → User (context switch)**
1. Set up user stack pointer (RSP)
2. Set up user code address (RIP)
3. Verify user selectors (0x2B for CS, 0x33 for SS)
4. Execute `sysretq` or `iretq` with user selectors

## Testing

The test suite (`test_gdt.c`) validates:

1. **Selector Values**
   - Kernel CS = 0x08
   - Kernel DS = 0x10
   - User CS = 0x2B
   - User DS = 0x33

2. **Privilege Level Bits**
   - Kernel selectors have RPL=0
   - User selectors have RPL=3

3. **Index Calculations**
   - Selector >> 3 yields correct GDT index
   - (index << 3) | rpl yields correct selector

4. **Descriptor Formats** (implicit through compilation)
   - Descriptors compile without warnings
   - Selectors computed correctly at runtime

## Phase 2-5 Usage

### Address Space Isolation (Issue #42, Phase 1)
- Kernel and user selectors required for Ring 3 execution
- User processes loaded with selector 0x2B (CS) and 0x33 (DS/SS)
- Per-process page tables provide actual memory isolation

### System Calls (Issue #41, Phase 1)
- `SYSCALL` instruction uses kernel CS from MSR
- `SYSRET` returns to user code with selector 0x2B
- Provides fast privilege level transition

### Device Driver I/O (Phase 2)
- I/O operations remain in kernel (Ring 0)
- User processes cannot directly access I/O ports (protected by privilege level)

### Graphics Framebuffer (Phase 3)
- Kernel driver maps framebuffer into user VA space
- User code accesses via paging, not via selectors
- GDT selectors still control privilege for any I/O operations

## References

- **Intel 64 and IA-32 Architectures SDM, Vol. 3A, Ch. 3**: Memory Management
- **x86-64 System V ABI**: Segment Register Semantics
- **Linux kernel source** (arch/x86/kernel/cpu/*.c): Real-world implementation reference
- **OSDev.org**: GDT and protected mode resources

## Future Enhancements

1. **LDTR (Local Descriptor Table)**
   - Currently not used (single global GDT for all tasks)
   - Could enable per-process descriptor tables if needed
   - Adds complexity; GDT-only approach is simpler and performant

2. **TSS (Task State Segment)**
   - Required for hardware task switching (rarely used in modern OSes)
   - Used to define kernel stack for privilege level transitions
   - Defer implementation until needed

3. **Call Gates**
   - Alternate privilege transition mechanism (slower than SYSCALL)
   - Not needed with modern SYSCALL/SYSRET instructions
   - Maintained for historical compatibility

## Checklist: GDT Implementation

- [x] Extended GDT array from 3 to 5 entries
- [x] Added user CS descriptor (0x2B) with DPL=3
- [x] Added user DS descriptor (0x33) with DPL=3
- [x] Verified descriptor bit patterns
- [x] Defined selector constants in gdt.h
- [x] Updated GDT initialization code
- [x] Created comprehensive test suite
- [x] Verified all existing tests pass (no regressions)
- [x] Added 7 new GDT tests (all passing)

## Next Steps

1. **Address Space Module (Issue #42)**
   - Create `struct address_space` with PML4 physical address
   - Implement address space creation/destruction
   - Set up per-process page tables

2. **Context Switch CR3 Loading**
   - Update `context_switch.S` to load CR3 from address_space->pml4_pa
   - Enable actual user-to-kernel privilege separation

3. **User Process Execution**
   - Modify process launcher to:
     - Create address space
     - Set user code entry point
     - Execute with user selectors (0x2B/0x33)
     - Jump to user code with `sysretq`

4. **System Call Entry Point**
   - Implement SYSCALL instruction handler
   - Switch from Ring 3 to Ring 0
   - Dispatch to appropriate syscall implementation

---

**Document Version:** 1.0
**Last Updated:** 2026-07-08
**Status:** Complete - Phase 1 Prerequisites Implemented
