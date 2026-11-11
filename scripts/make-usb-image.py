#!/usr/bin/env python3
"""Build a bootable UEFI disk image (GPT + one FAT32 EFI System Partition) from out/uefi.

Usage: python scripts/make-usb-image.py [out/winclone-usb.img] [--size-mb N] [--src DIR]

The image holds what `make CC=clang uefi-loader` staged: EFI/BOOT/BOOTX64.EFI,
EFI/BOOT/KERNEL.BIN and APPS/*. Write it to a USB stick (Rufus in "DD Image" mode,
Win32 Disk Imager, or `dd`) and boot the PC from it in UEFI mode. Needs no host tools:
standard library only. See docs/USB_IMAGE.md.
"""
import importlib.util
import os
import struct
import sys
import uuid
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SECTOR = 512
PART_START = 2048               # 1 MiB alignment
GPT_TAIL = 64                   # sectors kept free at the end for the backup GPT

ESP_TYPE = uuid.UUID("C12A7328-F81F-11D2-BA4B-00A0C93EC93B")


def load_fat32_writer():
    """Reuse the FAT32 writer of make-disk-image.py (its file name has a dash)."""
    path = os.path.join(ROOT, "scripts", "make-disk-image.py")
    spec = importlib.util.spec_from_file_location("make_disk_image", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def geometry(size_mb, spc):
    total = size_mb * 1024 * 1024 // SECTOR
    part_sectors = total - PART_START - GPT_TAIL
    reserved, nfats = 32, 2
    fat_size = 1
    while True:
        clusters = (part_sectors - reserved - nfats * fat_size) // spc
        needed = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
        if needed <= fat_size:
            break
        fat_size = needed
    if clusters < 65525:
        sys.exit("image too small for FAT32 (%d clusters): use a larger --size-mb" % clusters)
    return dict(total=total, part_sectors=part_sectors, spc=spc, reserved=reserved,
                nfats=nfats, fat_size=fat_size, clusters=clusters)


def add_tree(fs, mod, host_dir, parent_cluster, cluster):
    """Write host_dir as the FAT directory starting at `cluster` (already allocated)."""
    entries = []
    if cluster != fs.root:
        entries.append(bytes(fs.entries_for(".", b".".ljust(11), 0x10, cluster, 0)[-1]))
        entries.append(bytes(fs.entries_for("..", b"..".ljust(11), 0x10, parent_cluster, 0)[-1]))
    else:
        label = bytearray(32)
        label[0:11] = b"WINCLONE   "
        label[11] = 0x08
        entries.append(bytes(label))
    for name in sorted(os.listdir(host_dir)):
        path = os.path.join(host_dir, name)
        base, dot, ext = name.upper().partition(".")
        if len(base) > 8 or len(ext) > 3 or "." in ext:
            sys.exit("%s is not an 8.3 name; rename it" % path)
        sfn = mod.sfn_of(base, ext)
        if os.path.isdir(path):
            child = fs.alloc()
            add_tree(fs, mod, path, cluster, child)
            entries += fs.entries_for(name, sfn, 0x10, child, 0)
        else:
            data = open(path, "rb").read()
            entries += fs.entries_for(name, sfn, 0x20, fs.write_chain(data), len(data))
    fs.add_dir(entries, cluster)


def gpt_header(my_lba, alt_lba, total, entries_lba, entries_crc, disk_guid):
    h = bytearray(92)
    h[0:8] = b"EFI PART"
    struct.pack_into("<IIII", h, 8, 0x00010000, 92, 0, 0)       # revision, size, crc (later), reserved
    struct.pack_into("<QQQQ", h, 24, my_lba, alt_lba, 34, total - 34)
    h[56:72] = disk_guid.bytes_le
    struct.pack_into("<QIII", h, 72, entries_lba, 128, 128, entries_crc)
    struct.pack_into("<I", h, 16, zlib.crc32(bytes(h)) & 0xFFFFFFFF)
    return bytes(h).ljust(SECTOR, b"\x00")


def add_gpt(img, geo):
    total = geo["total"]
    disk_guid = uuid.uuid4()
    entry = bytearray(128)
    entry[0:16] = ESP_TYPE.bytes_le
    entry[16:32] = uuid.uuid4().bytes_le
    struct.pack_into("<QQQ", entry, 32, PART_START, PART_START + geo["part_sectors"] - 1, 0)
    entry[56:56 + 22] = "EFI System Partition".encode("utf-16-le")
    table = bytes(entry).ljust(128 * 128, b"\x00")
    crc = zlib.crc32(table) & 0xFFFFFFFF

    mbr = bytearray(SECTOR)                                     # protective MBR
    mbr[446 + 1:446 + 4] = b"\x00\x02\x00"
    mbr[446 + 4] = 0xEE
    mbr[446 + 5:446 + 8] = b"\xFF\xFF\xFF"
    struct.pack_into("<II", mbr, 446 + 8, 1, min(total - 1, 0xFFFFFFFF))
    mbr[510:512] = b"\x55\xAA"
    img[0:SECTOR] = mbr

    img[SECTOR:2 * SECTOR] = gpt_header(1, total - 1, total, 2, crc, disk_guid)
    img[2 * SECTOR:2 * SECTOR + len(table)] = table
    backup = (total - 33) * SECTOR
    img[backup:backup + len(table)] = table
    img[(total - 1) * SECTOR:total * SECTOR] = gpt_header(total - 1, 1, total, total - 33, crc, disk_guid)


def main():
    args = sys.argv[1:]
    size_mb, src = 128, os.path.join(ROOT, "out", "uefi")
    out = os.path.join(ROOT, "out", "winclone-usb.img")
    rest = []
    i = 0
    while i < len(args):
        if args[i] == "--size-mb":
            size_mb = int(args[i + 1]); i += 2
        elif args[i] == "--src":
            src = args[i + 1]; i += 2
        else:
            rest.append(args[i]); i += 1
    if rest:
        out = rest[0]
    for need in ("EFI/BOOT/BOOTX64.EFI", "EFI/BOOT/KERNEL.BIN", "APPS"):
        if not os.path.exists(os.path.join(src, need)):
            sys.exit("%s is missing: run `make CC=clang uefi-loader` first" % os.path.join(src, need))

    mod = load_fat32_writer()
    geo = geometry(size_mb, 2)
    fs = mod.Fat32(geo)
    mod.PART_START = PART_START
    # Only what the loader needs: EFI/ and APPS/ (skip build leftovers such as *.obj).
    root_entries = []
    label = bytearray(32)
    label[0:11] = b"WINCLONE   "
    label[11] = 0x08
    root_entries.append(bytes(label))
    for name in ("EFI", "APPS"):
        child = fs.alloc()
        add_tree(fs, mod, os.path.join(src, name), 0, child)
        root_entries += fs.entries_for(name, mod.sfn_of(name), 0x10, child, 0)
    fs.add_dir(root_entries, fs.root)

    img = bytearray(fs.finish("WINCLONE"))
    add_gpt(img, geo)
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "wb") as f:
        f.write(img)
    print("created %s (%d MiB, GPT + FAT32 EFI System Partition, %d clusters used)"
          % (out, size_mb, fs.next - 2))


if __name__ == "__main__":
    main()
