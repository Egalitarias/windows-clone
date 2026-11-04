# VM Testing Workflow

This document describes how to test the WinClone bootloader and installer in a QEMU VM.

## Quick Start

### Prerequisites

- QEMU with x86_64 support: `qemu-system-x86_64`
- OVMF firmware files:
  - `OVMF_CODE.fd` (UEFI code)
  - `OVMF_VARS.fd` (UEFI variables)
- UEFI build tools: `clang`, `lld-link`, `llvm-objcopy`

### Basic Boot Test

To boot the UEFI bootloader without installation:

```bash
make qemu-run
```

The kernel console is also mirrored to QEMU serial stdio, so the boot and fault-test output is visible in the terminal.

This will:
1. Build the UEFI bootloader (`BOOTX64.EFI`)
2. Build the kernel image (`KERNEL.BIN`)
3. Copy them to the UEFI directory
4. Start QEMU with OVMF firmware
5. Boot directly to the bootloader

**Expected behavior:**
- UEFI firmware loads
- `BOOTX64.EFI` runs and prints:
  - "WinClone UEFI loader"
  - "Loading \\EFI\\BOOT\\KERNEL.BIN ..."
  - "Kernel image validated."
  - "Handing off to kernel entrypoint..."
  - Kernel runs (currently just halts)

### Interactive VM Shell

To boot the prebuilt test disk image interactively:

```bash
make vm-shell
```

This starts QEMU with:
- A bootable raw disk image that contains `BOOTX64.EFI`, `KERNEL.BIN`, and `install.sh`
- A normal QEMU window for boot verification

**To exit QEMU:** Close the QEMU window, or open the QEMU monitor (`Ctrl+Alt+2`) and run `quit`.

## Full Test Workflow

The complete test cycle (boot → install → restart → verify) requires:

1. **Build the test disk:** `make vm-disk`
2. **Boot the VM:** `make vm-shell`
3. **Verify boot:** The bootloader loads and hands off to the kernel
4. **Run the installer:** Mount the test disk's ESP on the host and run the staged `install.sh`
5. **Restart:** Reboot QEMU
6. **Verify:** Confirm the bootloader loads again from the installed disk

### Manual Test Steps

```bash
# 1. Build the test disk
make vm-disk

# 2. Start the interactive VM
make vm-shell

# 3. Mount the disk image's ESP on the host and run the staged installer
# (the installer script is staged at the ESP root as /install.sh)

# 4. Restart the VM
# Close the QEMU window, or open the QEMU monitor (Ctrl+Alt+2) and run 'quit', then restart

# 5. Verify by running qemu-run again
make qemu-run
```

## Automated VM Test

To run a fully automated test cycle:

```bash
make vm-test
```

## Exception Handler Fault Test

To intentionally trigger a page fault and verify the handler path:

```bash
make fault-test
```

This boots the kernel with a deliberate invalid-opcode trap after the IDT and paging setup so the exception handler should print the fault and halt.

The serial log is written to `out/fault-test-serial.log`.

**Note:** Current implementation starts the interactive VM shell. Full automation requires:
- A host-side mount helper for the ESP
- Automating the install.sh invocation against the test disk
- Automated restart and verification

## Creating a Disk Image

To create a pre-configured disk image for testing:

```bash
make vm-disk
```

This creates `out/vm-disk.img` (512MB), a bootable GPT/FAT32 disk image with the UEFI loader and installer staged in the ESP.

**To use the disk in QEMU:**

```bash
qemu-system-x86_64 \
  -machine q35 \
  -m 512M \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=out/OVMF_VARS.fd \
  -drive format=raw,file=fat:rw:out/uefi \
  -drive if=none,format=raw,file=out/vm-disk.img,id=vmdisk \
  -device virtio-blk-pci,drive=vmdisk,bootindex=0
```

## Troubleshooting

### OVMF firmware not found

**Error:** `Missing OVMF code image: /usr/share/OVMF/OVMF_CODE.fd`

**Solution:** Provide the correct paths to OVMF files:

```bash
make qemu-run \
  OVMF_CODE=/path/to/OVMF_CODE.fd \
  OVMF_VARS_TEMPLATE=/path/to/OVMF_VARS.fd
```

### QEMU not found

**Error:** `Missing emulator: qemu-system-x86_64`

**Solution:** Install QEMU with x86_64 support:

```bash
# macOS
brew install qemu

# Ubuntu/Debian
sudo apt-get install qemu-system-x86
```

### Compiler/Linker errors

**Error:** `Missing PE/COFF linker: lld-link`

**Solution:** Install llvm tools:

```bash
# macOS
brew install llvm

# Ubuntu/Debian
sudo apt-get install llvm
```

## Next Steps

1. **Enhance the installer:** Add partition detection and creation
2. **Add boot persistence:** Update UEFI boot order so the installed bootloader persists
3. **Test on real hardware:** Once stable, test on actual x86_64 systems
4. **Automate restart cycle:** Implement auto-restart testing in the test suite
5. **Add kernel functionality:** Extend kernel beyond just halting

## File Layout

```
src/
├── boot/
│   ├── uefi_main.c       # UEFI bootloader source
│   └── stage1.S          # Legacy boot stage 1 (unused in this test)
├── kernel/
│   └── uefi_kernel.S     # Kernel image with EFI handoff
├── installer/
│   └── install.sh        # Installer script (runs inside VM)
└── apps/
    └── textedit/
        └── editor.S      # Text editor (staged but not used in basic test)

out/
├── uefi/
│   └── EFI/BOOT/
│       ├── BOOTX64.EFI   # Compiled UEFI bootloader
│       └── KERNEL.BIN    # Compiled kernel image
├── OVMF_VARS.fd          # UEFI variables (created per boot)
└── vm-disk.img           # Optional disk image
```

## Makefile Targets

- `make uefi-loader` - Build bootloader and kernel only
- `make qemu-run` - Boot bootloader in QEMU
- `make vm-shell` - Interactive VM shell for manual testing
- `make vm-disk` - Create a bootable disk image
- `make vm-test` - Run automated test cycle (WIP)
- `make clean` - Clean all build artifacts
