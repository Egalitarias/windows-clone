# Syscall Mechanism Design: SYSCALL vs. INT vs. SYSENTER

## Executive Summary

This document analyzes three privilege-level transition mechanisms available on x86-64 and recommends **SYSCALL/SYSRET** for WinClone, with implementation details and design rationale.

| Mechanism | Speed | Compat | Complexity | Recommendation |
|-----------|-------|--------|------------|-----------------|
| SYSCALL/SYSRET | **Fast** | x86-64 only | Medium | ✅ **RECOMMENDED** |
| INT 0x80 | Slow | x86/x64 | Medium | Legacy, slower |
| SYSENTER/SYSEXIT | Fast | x86/x64 | High | Requires TSS, deprecated |

## Mechanism 1: INT Instruction (Interrupt)

### Overview

The INT instruction triggers a software interrupt, causing a privilege transition via the IDT (Interrupt Descriptor Table).

```asm
mov $SYS_read, %rax    # Syscall number in RAX
mov $fd, %rdi          # Arg 1 in RDI
mov $buf, %rsi         # Arg 2 in RSI
mov $count, %rdx       # Arg 3 in RDX
int $0x80              # Trigger interrupt vector 128 (or other)
# Returns to next instruction with result in RAX
```

### Hardware Behavior

```
User Code (Ring 3) → INT 0x80 → CPU:
  1. Save user flags to stack (RFLAGS)
  2. Look up interrupt vector 128 in IDT
  3. Get kernel code selector (CS) and handler address (RIP) from IDT
  4. Switch privilege level (Ring 3 → Ring 0)
  5. Load kernel stack pointer from TSS (if privilege change)
  6. Save user code selector (CS), RIP, RFLAGS to kernel stack
  7. Jump to kernel handler address with new privilege

Kernel Handler (Ring 0) → IRETQ → CPU:
  1. Pop RFLAGS, RIP, CS from stack
  2. Switch privilege level back to Ring 3
  3. Jump to user code address
  4. Restore user RFLAGS
```

### Characteristics

**Advantages:**
- Works on all x86-64 systems (IRET support universal)
- Simple IDT-based mechanism
- No MSR setup required
- Legacy support (INT 0x80 well-established)

**Disadvantages:**
- **Slow:** ~5000+ CPU cycles (includes IDT lookup, flag saving, TSS access)
- Requires TSS for kernel stack switching
- Extra context saves (RFLAGS, segment registers)
- Modern OSes moving away from INT syscalls

**Performance Impact:**
- Syscall → ~10µs per call (at 1 GHz)
- For I/O-bound programs: 10-100k syscalls/sec → significant overhead
- Not recommended for high-frequency syscalls (e.g., `gettimeofday`, `read`)

### Code Example

```asm
# User-space syscall wrapper
syscall_read:
    mov $SYS_read, %rax       # Syscall number
    mov $fd, %rdi             # FD
    mov $buf, %rsi            # Buffer
    mov $count, %rdx          # Count
    int $0x80
    ret

# Kernel-space handler (in IDT)
# Vector 128 → points to: kernel_syscall_handler
kernel_syscall_handler:
    # Registers already saved by CPU
    # RAX contains syscall number
    # RDI, RSI, RDX, R10, R8, R9 contain arguments
    
    cmp $SYS_read, %rax
    jne .Lother_syscalls
    
    # Handle SYS_read
    call sys_read
    
.Lother_syscalls:
    # Handle other syscalls...
    
    iretq   # Return to user code
```

## Mechanism 2: SYSENTER/SYSEXIT (Legacy Fast Path)

### Overview

SYSENTER is a special instruction providing a faster privilege transition than INT, by skipping IDT lookup and directly jumping to a kernel handler.

```asm
mov $SYS_read, %rax    # Syscall number
mov $fd, %rdi          # Arguments...
mov $buf, %rsi
mov $count, %rdx
sysenter               # Fast kernel entry
```

### Hardware Behavior

```
User Code (Ring 3) → SYSENTER → CPU:
  1. Load kernel code selector from IA32_SYSENTER_CS MSR
  2. Load kernel entry point from IA32_SYSENTER_EIP MSR
  3. Load kernel stack pointer from IA32_SYSENTER_ESP MSR
  4. Switch privilege level (Ring 3 → Ring 0)
  5. Atomically:
     - Set CS = MSR CS (kernel)
     - Set RIP = MSR EIP (kernel handler address)
     - Set RSP = MSR ESP (kernel stack)
  6. Jump to kernel handler (no flag/register saves)

Kernel Handler (Ring 0) → SYSEXIT → CPU:
  1. Restore user stack pointer: RSP = saved user RSP
  2. Restore user code: RIP = saved user RIP (from R11 or stack)
  3. Atomically:
     - Set CS = user code selector
     - Set privilege level (Ring 0 → Ring 3)
     - Set RSP = user stack pointer
  4. Jump to user code
```

### MSR Setup Required

SYSENTER requires kernel to configure three MSRs:

```c
#define IA32_SYSENTER_CS   0x174
#define IA32_SYSENTER_ESP  0x175
#define IA32_SYSENTER_EIP  0x176

// During kernel initialization:
wrmsr(IA32_SYSENTER_CS,  GDT_KERNEL_CS);      // Kernel code selector (0x08)
wrmsr(IA32_SYSENTER_EIP, &kernel_entry);      // Kernel handler address
wrmsr(IA32_SYSENTER_ESP, &kernel_stack);      // Kernel stack pointer
```

### Characteristics

**Advantages:**
- Faster than INT (~1000-2000 cycles vs. 5000+)
- Direct MSR-based jump (no IDT lookup)
- No TSS access needed
- ~3-5µs per syscall (vs. ~10µs for INT)

**Disadvantages:**
- **Requires significant CPU setup**
  - Three MSRs must be configured
  - Kernel stack pointer precomputed (not per-process)
  - Shared kernel stack (not per-task in this OS)
  
- **No standard user-to-kernel transition**
  - Architecture-specific behavior
  - Different on Intel vs. AMD
  - Requires careful RSP/RIP management
  
- **Deprecated on x86-64**
  - SYSCALL/SYSRET preferred (faster, simpler)
  - SYSENTER exists for backward compatibility
  - Intel moving away in favor of SYSCALL
  
- **Privilege level return mechanism unclear**
  - SYSENTER doesn't save user state
  - Must manually save RIP, RFLAGS in kernel code
  - Requires custom return path (SYSEXIT or IRETQ)

### Code Complexity

```asm
# User-space (simple)
syscall_read:
    mov $SYS_read, %rax
    # Load arguments into RDI, RSI, RDX
    sysenter              # Fast kernel entry
    # On return, result in RAX
    ret

# Kernel-space (complex!)
kernel_sysenter_entry:
    # NOTE: No registers saved by CPU!
    # User RIP unknown (in stack or user RSP)
    # User RFLAGS unknown (discarded)
    # User RSP unknown (overwritten by kernel)
    
    # Must manually save everything
    mov %rsp, %rax              # Save kernel stack ptr
    mov %gs:0, %rsp             # Load per-CPU kernel stack
    push %rax                   # Save user RSP
    
    # Save all GP registers
    push %r15; push %r14; push %r13; push %r12
    push %rbx; push %rbp
    
    # Do syscall dispatch
    call syscall_dispatcher
    
    # Restore user RSP
    mov (%rsp), %rax
    mov %rax, %rsp
    
    # Return to user code via SYSEXIT
    # BUT sysexit doesn't restore flags!
    # Must use IRETQ instead (defeating the purpose)
```

## Mechanism 3: SYSCALL/SYSRET (Modern Standard) ⭐

### Overview

SYSCALL is the modern, efficient x86-64 instruction for privilege transitions. It's simpler than SYSENTER and faster than INT, with standard ABI semantics.

```asm
mov $SYS_read, %rax    # Syscall number
mov $fd, %rdi          # Arg 1
mov $buf, %rsi         # Arg 2
mov $count, %rdx       # Arg 3
mov $arg4, %r10        # Arg 4 (R10, not R8 on x86-64)
mov $arg5, %r8         # Arg 5
mov $arg6, %r9         # Arg 6
syscall                # Fast kernel entry
# Returns to next instruction with result in RAX
```

### Hardware Behavior

```
User Code (Ring 3) → SYSCALL → CPU:
  1. Load kernel code selector from IA32_LSTAR (Lower StarT ARguments) MSR
  2. Load kernel entry point from same MSR (RIP part)
  3. Load RFLAGS mask from IA32_FMASK MSR
  4. CPU atomically:
     - Save user code selector (CS) in implicit register
     - Save user RIP in RCX register
     - Save user RFLAGS in R11 register
     - Set CS = kernel code selector (0x08)
     - Set RIP = kernel handler address
     - Disable interrupts: RFLAGS.IF = 0
     - Clear user-mode trap flags
  5. Jump to kernel handler (privilege level implicit, hardware handles)

Kernel Handler (Ring 0) → SYSRET → CPU:
  1. Load user code address from RCX (or R11, depends on SYSRETQ)
  2. Load user flags from R11
  3. CPU atomically:
     - Set CS = user code selector (0x23, computed as kernel_CS + 16)
     - Set RIP = value in RCX
     - Set RFLAGS = value in R11
     - Switch privilege level (Ring 0 → Ring 3)
     - Restore interrupts if needed
  4. Jump to user code (privilege restored)
```

### MSR Setup Required

Three MSRs configure SYSCALL behavior:

```c
#define IA32_STAR      0xC0000081   // Star of SYSCALL (selector info)
#define IA32_LSTAR     0xC0000082   // Long mode SYSCALL entry point
#define IA32_FMASK     0xC0000084   // RFLAGS mask for SYSCALL

// During kernel initialization:
uint64_t star = ((uint64_t)GDT_KERNEL_CS << 32) |
                ((uint64_t)GDT_USER_CS - 8 << 48); // Sysret selector
wrmsr(IA32_STAR,   star);                          // Set selectors
wrmsr(IA32_LSTAR,  &kernel_syscall_entry);         // 64-bit entry point
wrmsr(IA32_FMASK,  X86_EFLAGS_IF);                 // Mask interrupt flag
```

### Characteristics

**Advantages:**
- ✅ **Fastest syscall mechanism** (~500-1000 cycles)
- ✅ **x86-64 standard** (every modern CPU supports)
- ✅ **Minimal CPU setup** (just three MSRs)
- ✅ **Automatic state saving** (RCX, R11 saved by CPU)
- ✅ **ABI standard** (calling convention well-defined)
- ✅ **High frequency syscalls viable** (gettimeofday, sched_yield)
- ✅ **Simpler than SYSENTER** (no manual RSP management)

**Disadvantages:**
- x86-64 only (doesn't work on 32-bit x86)
- Requires MSR writes (privileged operation)
- Must preserve RCX and R11 (not available for user parameters)

### x86-64 ABI Syscall Convention

```
Arguments in order:
  RDI = arg1
  RSI = arg2
  RDX = arg3
  R10 = arg4   (not R8, because R8 would be clobbered by SYSCALL!)
  R8  = arg5
  R9  = arg6

Special registers:
  RAX = syscall number (input), return value (output)
  RCX = user RIP (saved by SYSCALL, must restore before SYSRET)
  R11 = user RFLAGS (saved by SYSCALL, must restore before SYSRET)

Return value:
  RAX = return value (or -errno on error)
  RDX = (sometimes used, e.g., for 128-bit returns)
```

### Code Example

```asm
# User-space syscall wrapper
syscall_read:
    mov $SYS_read, %rax       # Syscall number
    mov $fd, %rdi             # FD (arg1)
    mov $buf, %rsi            # Buffer (arg2)
    mov $count, %rdx          # Count (arg3)
    syscall                   # CPU saves RCX (RIP), R11 (RFLAGS)
    ret                       # Kernel has restored RCX, R11

# Kernel-space handler
kernel_syscall_entry:
    # At this point:
    # - Privilege level: Ring 0
    # - RCX = user RIP (return address)
    # - R11 = user RFLAGS
    # - RAX = syscall number
    # - RDI, RSI, RDX, R10, R8, R9 = arguments
    # - Interrupts disabled (IF=0)
    
    # Dispatch to syscall handler
    cmp $SYS_read, %rax
    jne .Lother_syscalls
    
    call sys_read             # RDI=fd, RSI=buf, RDX=count
    # Result in RAX
    
    # Return to user code
    sysretq                   # CPU restores privilege, RIP from RCX, RFLAGS from R11
```

## Comparison Table

| Feature | INT 0x80 | SYSENTER | SYSCALL ⭐ |
|---------|----------|----------|-----------|
| **Speed (cycles)** | 5000+ | 1000-2000 | 500-1000 |
| **Time per syscall** | ~10µs | ~3-5µs | ~1-2µs |
| **x86-64 Support** | Yes | Yes | Yes |
| **Automatic State Save** | Yes (RFLAGS, CS, RIP) | No | Yes (RCX, R11) |
| **MSR Setup** | None | 3 MSRs | 3 MSRs |
| **TSS Required** | Yes | Maybe | No |
| **Complexity** | Medium | High | Low |
| **Recommended** | No | No | **YES** |

## Performance Analysis

For a typical I/O-bound application (100k syscalls/sec):

```
INT 0x80:    100,000 syscalls × 10µs = 1 second overhead
SYSENTER:    100,000 syscalls × 4µs  = 0.4 seconds overhead
SYSCALL:     100,000 syscalls × 1µs  = 0.1 seconds overhead

Improvement: 10× faster than INT, 4× faster than SYSENTER
```

For high-frequency queries (gettimeofday loop, 1 million calls):

```
INT 0x80:    1,000,000 × 10µs = 10 seconds
SYSENTER:    1,000,000 × 4µs  = 4 seconds
SYSCALL:     1,000,000 × 1µs  = 1 second

Gain: 10× speedup with SYSCALL
```

## Design Recommendation: SYSCALL/SYSRET ⭐

### Rationale

1. **Performance**: ~10× faster than INT, ~4× faster than SYSENTER
2. **Simplicity**: Fewer edge cases, cleaner semantics
3. **Standard**: x86-64 ABI, all modern OSes use this
4. **Supported**: Every x86-64 CPU implements SYSCALL (AMD K6+, Intel P4+)
5. **Future-proof**: SYSENTER/SYSEXIT deprecated in modern hardware

### Implementation Strategy

#### 1. MSR Configuration (during kernel initialization)

```c
static void syscall_init(void) {
    uint64_t star;
    
    // IA32_STAR: selectors for SYSCALL/SYSRET
    // Lower 32 bits: kernel code selector (bits 32-47)
    // Upper 16 bits: user code selector - 8 (bits 48-63)
    // (SYSRET automatically adds 8 for user DS)
    
    star = ((uint64_t)GDT_KERNEL_CS << 32) |   // Kernel CS (0x08 << 32)
           ((uint64_t)(GDT_USER_CS - 8) << 48); // User CS - 8 (0x23 << 48)
    
    wrmsr(IA32_STAR,   star);
    wrmsr(IA32_LSTAR,  &syscall_entry);        // 64-bit entry point
    wrmsr(IA32_FMASK,  X86_EFLAGS_IF);         // Mask interrupts during SYSCALL
    
    // Enable SYSCALL instruction (set SCE bit in EFER MSR)
    uint64_t efer = rdmsr(IA32_EFER);
    efer |= (1UL << 0);  // SYSCALL Enable bit
    wrmsr(IA32_EFER, efer);
}
```

#### 2. Syscall Entry Point (kernel-space assembly)

```asm
.global syscall_entry
syscall_entry:
    # At this point:
    # - Running in Ring 0
    # - RCX = user RIP (saved by CPU)
    # - R11 = user RFLAGS (saved by CPU)
    # - Interrupts disabled (IF=0)
    
    # Switch to kernel stack (if needed)
    swapgs                        # Swap GS base (user ↔ kernel)
    mov %gs:kernel_rsp, %rsp     # Load kernel stack
    
    # Enable interrupts (safe now that we have kernel stack)
    sti
    
    # Save user context for potential signal delivery
    push %r11                      # User RFLAGS
    push %rcx                      # User RIP
    push %rax                      # Syscall number
    
    # Dispatch to syscall handler
    cmp $NR_SYSCALLS, %rax
    jge .Lsyscall_error
    
    # Load syscall handler address from table
    lea syscall_table, %r8
    mov (%r8, %rax, 8), %r8
    jmp *%r8
    
.Lsyscall_error:
    mov $-ENOSYS, %rax
    
.Lsyscall_return:
    # At this point, RAX contains return value
    # Restore user RIP and RFLAGS
    pop %rax                       # Syscall number (discard)
    pop %rcx                       # User RIP
    pop %r11                       # User RFLAGS
    
    # Disable interrupts before returning to user
    cli
    
    # Swap GS back to user base
    swapgs
    
    # Atomic return to user code (privilege level drop)
    sysretq
```

#### 3. Syscall Handler Registration

```c
typedef long (*syscall_handler_t)(void);

static syscall_handler_t syscall_table[NR_SYSCALLS] = {
    [SYS_read]    = &sys_read,
    [SYS_write]   = &sys_write,
    [SYS_exit]    = &sys_exit,
    [SYS_open]    = &sys_open,
    // ... more syscalls
};
```

#### 4. User-Space Syscall Wrapper

```c
// In libc or kernel API header
#define syscall(nr, arg1, arg2, arg3, arg4, arg5, arg6) ({    \
    long ret;                                                   \
    register long _rdi asm("rdi") = (long)(arg1);              \
    register long _rsi asm("rsi") = (long)(arg2);              \
    register long _rdx asm("rdx") = (long)(arg3);              \
    register long _r10 asm("r10") = (long)(arg4);              \
    register long _r8  asm("r8")  = (long)(arg5);              \
    register long _r9  asm("r9")  = (long)(arg6);              \
    register long _rax asm("rax") = (long)(nr);                \
    asm volatile("syscall"                                      \
        : "=a" (ret)                                            \
        : "0" (_rax), "r" (_rdi), "r" (_rsi), "r" (_rdx),      \
          "r" (_r10), "r" (_r8), "r" (_r9)                     \
        : "rcx", "r11", "memory");                             \
    ret;                                                        \
})
```

## Alternative: Hybrid Approach

For compatibility, kernel could support both INT and SYSCALL:

```c
// At initialization:
syscall_init();    // Setup SYSCALL/SYSRET (fast path)
idt_init();        // Setup INT 0x80 handler (slow path, fallback)

// IDT entry for vector 128:
idt[128] = {
    .base_low   = (uint32_t)&int_0x80_handler,
    .selector   = GDT_KERNEL_CS,
    .flags      = IDT_GATE_TYPE_TRAP | IDT_GATE_DPL_3,
    // ...
};
```

Benefits:
- Modern userspace uses SYSCALL (fast)
- Legacy/i386 compat via INT 0x80 (slow)
- Gradual migration possible

## Conclusion

**SYSCALL/SYSRET is the recommended mechanism** for WinClone:

1. ✅ **Fastest** (10× faster than INT)
2. ✅ **Simplest** (straightforward semantics)
3. ✅ **Standard** (x86-64 ABI)
4. ✅ **Universal** (every modern CPU)
5. ✅ **Future-proof** (standard going forward)

Implementation:
- 3 MSR writes during initialization
- ~50 lines of assembly for entry point
- Efficient, scalable syscall path
- Optional INT 0x80 for legacy compatibility

## References

- Intel 64 and IA-32 Architectures SDM, Vol. 2B: SYSCALL/SYSRET instructions
- AMD64 Architecture Programmer's Manual, Vol. 2: System Programming
- Linux kernel: arch/x86/entry/entry_64.S
- FreeBSD kernel: sys/amd64/amd64/support.S
- System V AMD64 ABI: https://wiki.osdev.org/System_V_ABI

---

**Document Version:** 1.0
**Last Updated:** 2026-07-08
**Status:** Design Complete - SYSCALL Recommended
