# Bootable USB image

`make CC=clang usb-image` builds `out/winclone-usb.img` from what `uefi-loader` staged
(`out/uefi`): a GPT disk with one FAT32 EFI System Partition holding
`EFI/BOOT/BOOTX64.EFI`, `EFI/BOOT/KERNEL.BIN` and `APPS/*.BIN`. It needs only Python
(`scripts/make-usb-image.py`, no mtools or sgdisk), so it works from Git Bash on Windows.
Options: `--size-mb N` (default 128, at least about 33 for FAT32), `--src DIR`.

## Writing it to a stick

All data on the stick is lost. Use one of:

- Rufus: pick the image, partition scheme GPT, target UEFI, and choose **DD Image** mode
  when asked.
- Win32 Disk Imager.
- Linux/macOS: `sudo dd if=out/winclone-usb.img of=/dev/sdX bs=4M conv=fsync`.

Then boot the PC from the stick in UEFI mode (turn Secure Boot off; the loader is unsigned).

## Checks

- `python scripts/fat32-ls.py out/winclone-usb.img --gpt` and `--check` validate the GPT and the
  FAT32 volume independently of the kernel.
- `python scripts/qemu-screenshot.py --usb out/winclone-usb.img --no-esp --no-data out.png 25`
  boots it as a USB stick in QEMU.
- The same image attached as NVMe (`--nvme`) boots and mounts as volume `WINCLONE`, and
  Notes and Files start from its `\APPS`.

## Known limit

The kernel has no USB storage driver yet. Booted from a USB stick, the UEFI firmware loads
the kernel fine, but the kernel then sees no disk with `\APPS`, so programs do not start
(Files and Notes show "Cannot start /APPS/..."). The same image on a SATA or NVMe disk works.
USB storage (xHCI plus mass storage) is the next step to make the stick usable on real
hardware; USB keyboard and mouse are needed there too.
