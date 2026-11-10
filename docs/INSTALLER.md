# Installer, partitioning and formatting

Source: `src/kernel/setup.c` (the Setup app), `format.c` / `format.h` (GPT writer and
FAT32 formatter), `nvme.c`, `disk.c`. Open it from **Start > Setup**.

## What Setup does

1. Finds the boot files on the medium the system booted from: any mounted volume that
   has `\EFI\BOOT\BOOTX64.EFI` and `\EFI\BOOT\KERNEL.BIN`. That medium's disk is marked
   "boot disk" and cannot be chosen. Disks under about 160 MB are marked "too small".
2. You pick a disk, press **Install to selected disk**, then confirm with
   **Erase and install** (the confirmation names the disk in red).
3. Reads both boot files into memory, so nothing depends on the source after this point.
   (The programs in `\APPS` are copied later, one at a time, from the same medium.)
4. Clears the first and last MiB of the disk and writes a fresh **GPT**: protective MBR,
   primary and backup headers, primary and backup entry arrays, CRC-32 on all of them,
   random disk and partition GUIDs.
   - partition 1: **EFI System Partition**, 100 MiB, 1 MiB aligned
   - partition 2: **data** (basic data type), the rest of the disk, 1 MiB aligned
5. Rescans partitions, formats both as **FAT32** (labels `ESP` and `WINCLONE`).
6. Mounts the new ESP, creates `\EFI\BOOT`, copies `BOOTX64.EFI` and `KERNEL.BIN`, and
   reads each back and compares it byte for byte. Then it creates `\APPS` on the ESP and
   copies every file of the medium's `\APPS` folder (Notes, Files, Paint, About and the
   other programs) the same way. **The desktop's Notes and Files are those programs, so a
   system without `\APPS` cannot open or edit files**: if the medium has no `\APPS`, Setup
   says so in the log and the installed system shows a message window instead.
7. Remounts everything. Files and Notes notice (via `fs_generation()`) and refresh or
   refuse to save through stale volume pointers.

Progress is shown live in the log. Afterwards shut down, remove the installation medium
and boot from the new disk: UEFI's fallback path `\EFI\BOOT\BOOTX64.EFI` starts it.

The Linux script (`src/installer/install.sh`) and the VM / VirtualBox disk images
(`make vm-disk`, `make vbox-disk`) ship the programs too: the installer package has an
`APPS` folder next to `EFI`, and they put it at `<ESP>/APPS`.

New files created in Notes go to the first volume without an `\EFI` folder (the data
partition), never the ESP.

## GPT writer (`gpt_write`)

Writes the backup structures first and the primary header plus protective MBR last, so an
interrupted write leaves the old table readable until the end. Validates alignment-free
bounds (inside the usable area) and rejects overlapping partitions.

## FAT32 formatter (`fat32_format`)

Cluster size follows the usual table for 512-byte sectors: 1 sector up to 260 MB, then 8,
16, 32 and 64 sectors per cluster at 8, 16 and 32 GB. Partitions need at least 65525
clusters (about 34 MB with 512-byte clusters), as FAT32 requires. It writes the boot sector
and backup (sector 6), FSInfo and backup (sector 7), clears both FATs completely, marks the
reserved entries and the root cluster, and writes the volume label record. The BPB records
the partition's start sector, so firmware and other tools accept the volume.

## NVMe driver (`nvme.c`)

Finds controllers by PCI class 01:08:02 and maps BAR0 (32- or 64-bit) uncached. It resets
the controller, builds the admin queue pair, enables it (64-byte SQ entries, 16-byte CQ
entries, 4 KiB pages), requests one I/O queue pair and creates it, then sends Identify for the
controller (model, MDTS) and the first active namespace. Reads and writes go through a 64 KB
bounce buffer using PRP1/PRP2 or a PRP list, are polled with the completion phase bit, and
one command is in flight at a time.

Not supported: namespaces with 4 KiB logical blocks (they are skipped), multiple namespaces
(only the first is exposed), interrupts, MSI-X, multiple queues, namespace management,
controllers whose minimum page size is larger than 4 KiB.

## Disk layer (`disk.c`)

One list of disks across SATA and NVMe (`sata0`, `nvme0`, ...). The block layer builds
partitions on top (`nvme0p1`) and `block_rescan()` re-reads partition tables after
partitioning without re-initialising controllers.

## Testing the installer in QEMU

```bash
python scripts/make-disk-image.py out/run/data.img --force      # optional data disk
python -c "f=open('out/run/target.img','wb'); f.truncate(512*1024*1024)"   # blank NVMe target
make CC=clang uefi-loader
python scripts/qemu-screenshot.py --nvme out/run/target.img out/run/s.png 16   # run Setup by hand,
# or script the clicks with monitor commands (see the examples in the git history)

python scripts/fat32-ls.py out/run/target.img --gpt             # validates headers, CRCs, overlaps
python scripts/fat32-ls.py out/run/target.img --part 1          # the ESP tree
python scripts/fat32-ls.py out/run/target.img --part 1 --check  # FAT consistency
# boot the result on its own:
python scripts/qemu-screenshot.py --no-esp --no-data --nvme out/run/target.img out/run/boot.png 18
```

`fat32-ls.py` is a separate implementation (Python, standard `zlib` CRC), so it checks the
kernel's GPT and FAT32 output independently.

## Not done yet

- Writing a boot menu entry in NVRAM (not needed: the fallback path boots).
- Secure Boot signing, a recovery partition, BitLocker-style encryption.
- Resizing, multi-boot, repair, or installing onto a disk that already holds data you want
  to keep.
- Installing from a medium whose volume is not FAT16/FAT32.
