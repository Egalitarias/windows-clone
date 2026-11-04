# Interrupts and timekeeping

Source: `src/kernel/irq.c`, `irq.h`, `irq_stubs.S`, `idt.c`, `timer.c`, `ps2.c`, `rtc.c`.

## IRQ layer

The 8259 PIC is remapped so IRQ0-7 are vectors 32-39 and IRQ8-15 are 40-47
(`pic.c`). Every PIC line has an entry stub (`irq_stub_N` in `irq_stubs.S`) that saves
the general registers and calls `irq_dispatch(N)` on a 16-byte aligned stack, then
returns with `iretq`.

`irq_dispatch` handles the PIC end-of-interrupt for you, including the slave PIC for
IRQ8-15, and ignores spurious IRQ7 / IRQ15 by checking the in-service register.

API (`irq.h`):

| Call | Purpose |
|---|---|
| `irq_init()` | mask every line and clear handlers |
| `irq_register(n, fn)` | set the handler for line `n` (assign at run time) |
| `irq_unmask(n)` / `irq_mask(n)` | enable / disable a line (unmasking IRQ8-15 also opens the cascade) |

Handlers run with interrupts disabled. Keep them short: move data into a buffer and
return. Never draw, allocate, or take locks in a handler.

## Who owns what

- **Vector 32 (timer):** during boot it points at `timer_interrupt_stub`, which drives
  the scheduler demo. When the desktop starts (after the scheduler has finished) the
  shell swaps it to `irq_stub_0` with `idt_set_handler()` and registers `timer_tick`.
- **Vectors 33-47:** installed at boot so a stray hardware interrupt can never be
  misreported as a CPU exception.

## Timer

The PIT runs at 1000 Hz. `timer_get_ticks()` counts ticks since `timer_init()`;
`timer_get_ms()` converts to milliseconds. The shell uses it for double-click timing
(400 ms, 5 px slop) and a once-a-second tick (`wm_tick()`) that refreshes the clock and
per-window `tick` callbacks (About's uptime).

## Real-time clock

`rtc_read_time()` reads hours, minutes and seconds from the CMOS RTC, handling BCD and
12/24-hour modes and retrying around the update-in-progress window. The time is what
the firmware or hypervisor provides (usually UTC); there is no time-zone support yet.

## PS/2 input

IRQ1 (keyboard) and IRQ12 (mouse) both drain the i8042 output buffer into a 256-entry
ring buffer, tagging each byte with its source from the status register. The ring has
one producer (the handlers) and one consumer (`ps2_poll`), so it needs no locking.
`ps2_poll` decodes scancodes and mouse packets in normal context.

## Idle

The shell's main loop ends each pass with `sti; hlt`, so the CPU sleeps until the next
interrupt (timer at 1 ms at worst, or input). `sti; hlt` is atomic: an interrupt
arriving right after `sti` still wakes the `hlt`. Measured in QEMU, the idle desktop
uses about 3% of one host core.

## Not done yet

- LAPIC / IOAPIC and HPET (the legacy PIC and PIT are fine for a single CPU).
- A tickless idle (the 1 kHz timer wakes the CPU 1000 times a second).
- Scheduler integration after boot: the scheduler is still a boot-time demo; the
  desktop runs on its own stack and does not use the scheduler's tick.
