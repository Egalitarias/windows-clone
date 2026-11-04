# Storage and file system

Source: `pci.c`, `ahci.c`, `nvme.c`, `disk.c`, `block.c`, `fat32.c`, `fs.c` (kernel) and
the Files app (`explorer.c`). Partitioning, formatting and the installer are in
`INSTALLER.md`. Host tools: `scripts/make-disk-image.py`, `scripts/fat32-ls.py`.

## Stack

```
Files app / Notes (explorer.c, apps.c)
        |
fs.c        mounted FAT32 volumes (fs_volume(i))
fat32.c     FAT16 and FAT32: long names, read, write, mkdir, delete
block.c     block devices = partitions (MBR / GPT) or whole disks
disk.c      one list of whole disks across controllers
ahci.c      AHCI SATA controller    nvme.c   NVMe controller   (both polled, DMA via a bounce buffer)
pci.c       PCI configuration space (ports 0xCF8 / 0xCFC)
```

## Why a native driver

UEFI Block I/O would be easier, but the kernel runs on its own page tables, so calling
firmware drivers can fault on device registers we have not mapped. A native driver works
the same on real hardware and has no firmware dependency after boot.

## PCI and AHCI

`pci_find_class(0x01, 0x06, 0x01, ...)` finds the AHCI controller; BAR5 (ABAR) is mapped
uncached with `paging_map_mmio()`. Per port the driver stops the command engine, gives
it a command list, FIS area and command table (one page), restarts it and issues
`IDENTIFY` for capacity and model. Reads and writes use `READ/WRITE DMA EXT` (48-bit LBA)
through a 64 KB physically contiguous bounce buffer, so callers can use ordinary memory.
Commands are issued one at a time on slot 0 and completed by polling `PxCI`.

Not done: ATAPI (CD/DVD), hot-plug, NCQ and interrupt-driven completion, USB mass
storage, error recovery beyond failing the command. NVMe is described in `INSTALLER.md`.

## Block layer

`block_init()` brings up AHCI, reads sector 0 of each disk and creates a block device
for every partition. Detected layouts: a bare FAT volume (no partition table), MBR
(primary partitions, extended containers skipped) and GPT (via the protective MBR).
Device names look like `disk1p1`. All sector numbers given to `block_read` /
`block_write` are relative to the start of the partition.

## FAT32

- Reads and writes **FAT16 and FAT32** with 512-byte sectors and any power-of-two cluster
  size. On FAT16 the root directory is a fixed region (addressed as directory 0): it works
  like any directory but cannot grow, so a full root reports an error. FAT12 is not
  supported.
- **Long file names** (VFAT): read, and written for any name that is not a plain upper-case
  8.3 name. `~N` aliases are generated and kept unique. Only ASCII is kept in long names;
  other characters show as `?`.
- Files: read, create, replace contents (frees the old cluster chain first).
- Directories: list, create (with `.` and `..`), delete when empty, grow when full,
  reuse deleted records.
- Both FAT copies are updated on every change; writes go straight to the disk (no
  write-back cache), so there is nothing to flush.
- Timestamps come from the CMOS clock.
- **Not supported:** FAT12, exFAT/NTFS, the FSInfo free-space hints (left as written by the formatter),
  file attributes other than directory, subdirectory renames, rename and move, and
  files larger than 4 GiB.

The driver is single-threaded and not reentrant: use it from the shell loop only.

## Files app

Drive list, folders first then A-Z, folder navigation, Up button / Backspace, New Folder,
Delete button / Delete key, arrow keys and Enter. `.txt .md .log .ini .c .h .csv .cfg`
open in Notes. Notes: Ctrl+S saves to the open file, or to the next free `NOTEn.TXT` on
the first volume for a new note. Text is capped at 4096 bytes in Notes; longer files are
truncated and flagged so you do not save over them.

## Testing

```bash
python scripts/make-disk-image.py out/run/data.img --force   # 128 MiB, MBR + FAT32 partition
make CC=clang uefi-loader
scripts/run-qemu-windows.sh --window     # attaches out/run/data.img on AHCI port 1
python scripts/fat32-ls.py out/run/data.img          # list the tree as the host sees it
python scripts/fat32-ls.py out/run/data.img --check  # FAT copies equal, no leaked or cross-linked clusters
```

`make clean` deletes `out/`, including the test image: recreate it afterwards.

`fat32-ls.py` is a separate implementation from the kernel driver, so it catches bugs
the kernel's own view of the disk would miss.

Kernel self-test: build with `make CC=clang FS_SELFTEST=1 uefi-loader`, boot, and read
the `[fstest]` lines on the serial log. It writes a 200 KB file, shrinks another,
creates 30 long-named files (directory growth), deletes half, nests directories and
checks name handling. Afterwards `fat32-ls.py` can verify the result byte for byte.
