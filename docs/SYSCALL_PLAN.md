# Syscall and User-Mode Implementation Plan

Status: draft. Companion to `SYSCALL_DESIGN.md` (mechanism choice) and `USERSPACE_LAYOUT.md` (address space).

## Starting point

- **Design is settled.** `SYSCALL_DESIGN.md` picks SYSCALL/SYSRET. `USERSPACE_LAYOUT.md` describes a per-process address space. Neither is implemented.
- **GDT has no TSS and the user selectors are wrong.** `gdt.h` defines `GDT_USER_CS 0x2B` and `GDT_USER_DS 0x33`. Those are indexes 5 and 6, but `g_gdt[]` has only 5 entries, so user code at index 3 would be `0x1B`. This needs fixing first.
- **Memory is identity-mapped, with a single address space.** `TCB` has `address_space`, `user_entry_point` and `user_stack_base` fields, but nothing uses them.
- **Tasks are kernel functions.** `scheduler_add_task(name, fn, arg)` is the only way to start one.
- **Apps are compiled into the kernel.** `src/apps` and `apps.c` hold them, and the kernel is a PIE with no relocations.
- **FAT32 is working.** User programs could be loaded from disk.

## Phases

### 1. Ring-3 plumbing (no syscalls yet) - DONE

- Fix the GDT layout. SYSRET needs user data at STAR base+8 and user code at base+16, so the order is kernel CS, kernel DS, user DS, user CS.
- Add a 64-bit TSS with `rsp0` and load it with `ltr`. Update `rsp0` on every context switch.
- Add an `enter_user(entry, user_rsp)` routine using `iretq`.
- Test: run a tiny user stub that loops. Check that a privileged instruction such as `hlt` raises #GP, and that the kernel survives.

### 2. Per-process address space

- Add user page tables with the U/S bit set, plus a shared kernel half. Keep a template PML4 for the kernel mappings.
- Move the kernel off the identity map and into the high half (`KERNEL_HIGH_HALF_BASE` already exists). This is the riskiest step, because the PIE kernel has no relocations and the page tables must be rebuilt.
- Alternative: keep the identity map for now and put user code in a separate low range with U/S set on its pages. This is weaker isolation, but much smaller. Recommended: start there and harden later.
- Add user-stack and heap setup with guard pages, reusing `stack_guard.c`.
- Handle page faults. A user fault should kill the process, not the kernel.

### 3. SYSCALL/SYSRET entry

- Set `EFER.SCE`, `STAR`, `LSTAR` and `SFMASK`.
- Write the entry stub. It does `swapgs` (or uses a per-CPU stack variable), switches to the kernel stack, saves registers, and dispatches through a table.
- Validate user pointers with `copy_from_user` and `copy_to_user`. Never trust a raw pointer.
- Start with a minimal set:
  - `exit`
  - `write` (to the console)
  - `yield`
  - `getpid`
  - `sleep`
  - `read_key`

### 4. Process loader

- Define a flat binary or minimal ELF64 static format for user programs. A flat binary is simpler to start with.
- Add a `process_create(path)` call that reads the file through `fs`, maps it, builds the stack, and creates a TCB with `address_space` set.
- Scheduler: when switching to a user task, load CR3 and set `rsp0`. Reap processes on exit.
- Add a small user-space library: syscall wrappers and `_start`.
- Build step: add a Makefile target that links user programs at their user base address and puts them on the FAT image.

### 5. File and window syscalls

- Add `open`, `read`, `write`, `close` and `readdir`, backed by FAT32.
- Add window syscalls so apps can draw into a window buffer. This is where a compositor would help.
- Port one existing app, such as Notes, to run as a real user process. It proves the whole path.

### 6. Hardening and tests

- Add tests for bad pointers, a stack overflow in user mode, unknown syscall numbers, and a user process that never yields (preemption).
- Update the docs and the README roadmap.

**Phase 1 status.** GDT reordered (user DS 0x1B, user CS 0x23, TSS 0x28), TSS loaded with `gdt_set_kernel_stack()`, `paging_map_user()` added, `user_enter()` in `usermode.c`, and the exception dump now shows RIP/CS/RSP. Verify with `make CC=clang USER_SELFTEST=1 uefi-loader` and boot: the user program runs `hlt` at CPL 3 and the kernel reports `Vector: 13`, `CS: 0x23 (ring 3)`. Known issue found on the way (not from this change): the `TESTS=1` build page-faults in the context-switch suite, so the GDT suite does not run there.

## Risks

- **The high-half move (phase 2).** Anything that holds physical or identity pointers will break, including the framebuffer, MMIO, AHCI and NVMe buffers, and DMA. That is why it is deferred.
- **No relocations in the kernel.** Static pointer tables must not be used in the syscall table. Use a `switch` or a flat array of function indexes.
- **Interrupts in the syscall path.** Preemption (`scheduler_on_timer_tick`) must handle a task that is in the middle of a syscall.

## Suggested first slice

Phases 1 and 3 with the identity map left alone: a ring-3 stub that calls `write` and `exit`. That proves the hard hardware parts. Phases 2 and 4 then build on it.
