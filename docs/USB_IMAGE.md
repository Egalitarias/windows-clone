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

## USB storage in the kernel

The kernel reads and writes USB sticks (`src/kernel/usbstor.c`): an xHCI controller driver plus
Bulk-Only Transport with SCSI commands, polled like the NVMe driver. A stick shows up as a disk
named `usb0`, so booting from the stick works: the kernel finds `\APPS` on it. Tested in QEMU
(`qemu-xhci` + `usb-storage`): boot from the image alone, Notes and Files start from it, and a
changed `TZ.CFG` is written back (checked with `scripts/fat32-ls.py --check`).

USB keyboards and mice work through the same driver: boot-protocol HID on interrupt endpoints,
polled from the desktop loop (`usbhid_poll`, same event type as the PS/2 driver, with software key
repeat). Tested in QEMU with `qemu-xhci` + `usb-kbd` + `usb-mouse` and the PS/2 controller turned off
(`scripts/qemu-screenshot.py --usb-hid`): the pointer moves and clicks, text is typed with Shift.

Limits: devices must sit in the PC's own USB ports (no hubs), 512-byte blocks only for storage,
disks up to 2 TiB (READ CAPACITY(10)), one controller command or transfer at a time, no hot plug
(devices are found at boot). Keyboards: US layout, no LEDs (Caps Lock works but its light does not).
Mice: three buttons and movement, no wheel; absolute pointers (touch screens, tablets) are ignored.
Not tried on real hardware yet. The kernel takes the controller over from the firmware (BIOS handoff);
on PCs that emulate a PS/2 keyboard through the BIOS that emulation goes away, which is why the
USB keyboard driver matters there.
