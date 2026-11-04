# VM Configuration for WinClone (Intel Mac)

This file defines a practical virtual machine profile for testing and deployment of this project.

## Current Project Reality (Important)

At this stage, the repo provides:

- Week 1 build artifacts and installer staging layout
- Week 2 UEFI bring-up (`BOOTX64.EFI`) tested via QEMU + OVMF

It does **not** yet provide a full end-user installer flow, so VM testing is primarily:

1. boot validation,
2. UEFI loader behavior,
3. repeatable deployment testing.

## Recommended VM Profile (Use This)

- **Architecture**: `x86_64` (matches current toolchain/targets)
- **Firmware**: `UEFI`
- **Machine type/chipset**: modern PCIe platform (`q35` equivalent where available)
- **vCPU**: `2` (minimum), `4` preferred
- **Memory**: `2048 MB` minimum, `4096 MB` preferred
- **Disk**: `32 GB` minimum, `64 GB` preferred (dynamically allocated)
- **Network**: `NAT` for normal internet access inside guest
- **Second network (optional)**: `Host-only` for stable host<->guest testing
- **Graphics**: default virtual adapter (no 3D acceleration required)
- **Secure Boot**: `Disabled` for now (loader is not signed yet)
- **TPM**: `Disabled` for now unless explicitly needed for experiments

## VirtualBox (Intel Mac) Settings

Create a new VM with:

- **Type**: Other Linux (64-bit) or your target guest base
- **Base Memory**: 4096 MB (or 2048 MB minimum)
- **Processors**: 2
- **Hard disk**: VDI, dynamically allocated, 64 GB (or 32 GB minimum)
- **System > Motherboard**:
  - Enable EFI (special OSes only): **On**
  - Boot order: Optical/ISO first, then Hard Disk
- **System > Acceleration**:
  - VT-x/AMD-V: **On**
  - Nested paging: **On**
- **Network**:
  - Adapter 1: NAT
  - Adapter 2 (optional): Host-only adapter

## VMware Fusion (Intel Mac) Equivalent Settings

- Firmware: **UEFI**
- CPUs: **2**
- Memory: **2-4 GB**
- Disk: **32-64 GB**, growable
- Network: **NAT** (plus optional host-only/private network)
- Secure Boot: **Off** for now

## Deployment/Test Checklist

Before deploying to this VM:

1. Confirm `make all` succeeds.
2. For UEFI bring-up, confirm `make uefi-loader` succeeds.
3. Ensure VM is configured for UEFI boot (not legacy BIOS).
4. Keep a clean snapshot named `base-clean` before each deployment test.

Per test cycle:

1. Revert VM to `base-clean`.
2. Deploy artifacts.
3. Boot and validate expected behavior.
4. Record result and create a new snapshot only for known-good milestones.

## Why These Defaults

These settings align with the repo's current x86_64 + UEFI bring-up path while staying lightweight enough for an Intel i5 Mac host.