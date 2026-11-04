# VM Testing Workflow

This directory contains scripts for automated QEMU VM testing of the bootloader, installer, and kernel.

## Quick Start

### Prerequisites

- QEMU installed (`qemu-system-x86_64`)
- OVMF firmware files (specify with `OVMF_CODE` and `OVMF_VARS_TEMPLATE`)
- mtools (for disk image creation): `brew install mtools gptfdisk`

### Running Tests

```bash
# Build disk image and run automated test suite
make vm-test

# Or run with custom OVMF paths
make vm-test OVMF_CODE=/path/to/OVMF_CODE.fd OVMF_VARS_TEMPLATE=/path/to/OVMF_VARS.fd
```

## Test Suite Overview

The `make vm-test` target runs 5 sequential validation tests:

1. **UEFI Boot** - Verify OVMF loads and reaches UEFI shell
2. **Disk Accessibility** - Verify EFI volumes are detected
3. **Kernel Boot** - Boot kernel from disk EFI/BOOT/KERNEL.BIN
4. **Test Suite Validation** - Verify kernel tests run (if TESTS=1 enabled)
5. **Boot Sequence Verification** - Confirm full boot produces output

Each test is timed (default 120s total) and outputs serial logs for inspection.

## Scripts

### `run-vm-test.sh`
Main QEMU automation script. Runs 5-stage boot validation:
- Creates temporary OVMF variables copy
- Boots QEMU with serial output capture
- Parses serial logs for success markers
- Reports detailed results with actual boot output

**Usage:**
```bash
bash scripts/run-vm-test.sh <disk_image> <ovmf_code> <ovmf_vars> [timeout_seconds]
```

**Example:**
```bash
bash scripts/run-vm-test.sh out/vm-disk.img OVMF/OVMF_CODE.fd out/OVMF_VARS.fd 120
```

### `test-vm-full.sh`
Wrapper script with colorized output and prerequisite checking:
- Validates QEMU, OVMF, and disk image are available
- Calls `run-vm-test.sh` with proper error handling
- Provides human-readable success/failure output

**Usage:**
```bash
bash scripts/test-vm-full.sh [disk_image] [ovmf_code] [ovmf_vars] [test_type]
```

### `verify-boot.sh`
Log parser for boot validation (currently unused, available for custom verification):

**Stages supported:**
- `boot` - Verify UEFI boot
- `uefi` - Verify UEFI console
- `installer` - Verify installer completion
- `system` - Verify kernel running

## Make Targets

### `make vm-disk`
Creates a 2GB bootable QEMU disk image with:
- UEFI System Partition (ESP) with BOOTX64.EFI and KERNEL.BIN
- Grub/root partition ready for installer
- Located at `out/vm-disk.img`

### `make vm-test`
Runs the full automated test suite:
- Ensures disk image exists (creates if needed)
- Validates QEMU and OVMF are available
- Runs `scripts/run-vm-test.sh` with timeout
- Reports pass/fail for each test stage

### `make vm-shell`
Launches interactive QEMU session for manual testing:
- Boots the VM disk in graphical mode
- Allows manual UEFI shell commands
- Useful for debugging boot issues
- Exit with Ctrl+A followed by X

## Troubleshooting

### QEMU timeout (kernel hangs)
Check serial output:
```bash
tail -50 /tmp/*/serial.log
```
Look for kernel boot messages, crash info, or incomplete initialization.

### "Missing OVMF" error
Install OVMF firmware:
```bash
# macOS
brew install ovmf

# Linux
sudo apt-get install ovmf
```

Then point Makefile to firmware location:
```bash
make vm-test OVMF_CODE=/path/to/OVMF_CODE.fd OVMF_VARS_TEMPLATE=/path/to/OVMF_VARS.fd
```

### Disk image creation fails
Ensure mtools is installed:
```bash
brew install mtools gptfdisk
```

And that `out/` directory exists and is writable.

## Test Output Example

```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
▶ Checking prerequisites...
✓ OVMF code: OVMF/OVMF_CODE.fd
✓ OVMF vars: out/OVMF_VARS.fd
✓ QEMU available
✓ Disk image: out/vm-disk.img (384M)

▶ Starting VM test suite...

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
Test 1/5: Initial boot to UEFI shell (30s timeout)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
QEMU PID: 12345
Serial output:
─────────────────────────────────────────────────
UEFI Secure Boot is not installed.
UEFI Secure Boot is enabled.
Default Boot Device Is Not Available
EFI Shell version 2.2
Current running mode: Long Mode (64-bit)
Available storage devices:
  Mapping(s):
  fs0: Alias(es):
      /dev/sda1 - Hard Drive: 2.00 GB
  fs1: Alias(es):
      /dev/sda2 - Hard Drive: 2.00 GB
Shell>
─────────────────────────────────────────────────
✓ Test 1: UEFI booted successfully

✓ ALL TESTS PASSED
```

## Integration with CI/CD

The test suite is designed for CI integration:
- Exit code 0 on full pass
- Exit code 1 on any failure
- Serial output logged to temporary files
- No interactive prompts or timeouts in Makefile target

Example GitHub Actions integration:
```yaml
- name: Run VM test suite
  run: make vm-test
  timeout-minutes: 5
```

## Future Enhancements

- [ ] Snapshot-based testing (skip disk recreation for repeated tests)
- [ ] Parallel test variants (TESTS=1 vs production kernel)
- [ ] Performance benchmarking (measure boot time)
- [ ] Boot sequence timeline visualization
- [ ] Automated installer testing (run install.sh in QEMU)
- [ ] Screenshot capture on failure
