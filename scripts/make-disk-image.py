#!/usr/bin/env python3
"""Create a raw disk image with an MBR and one FAT32 partition, for testing the
OS storage stack in QEMU.

Usage: python scripts/make-disk-image.py [out/run/data.img] [--size-mb N] [--force]

The image holds a few sample files (including a long file name and a
sub-directory) so the file explorer has something to show. Existing images are
left alone unless --force is given, so changes made by the OS persist between
runs. Standard library only.
"""
import os
import struct
import sys

SECTOR = 512
PART_START = 2048            # first partition sector (1 MiB alignment)


def build(size_mb):
    total_sectors = size_mb * 1024 * 1024 // SECTOR
    part_sectors = total_sectors - PART_START
    spc = 2 if size_mb >= 64 else 1
    reserved = 32
    nfats = 2
    # Solve for clusters / FAT size.
    fat_size = 1
    while True:
        data_sectors = part_sectors - reserved - nfats * fat_size
        clusters = data_sectors // spc
        needed = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
        if needed <= fat_size:
            break
        fat_size = needed
    return dict(total=total_sectors, part_sectors=part_sectors, spc=spc, reserved=reserved,
                nfats=nfats, fat_size=fat_size, clusters=clusters)


class Fat32:
    def __init__(self, geo):
        self.g = geo
        self.img = bytearray(geo["total"] * SECTOR)
        self.part0 = PART_START * SECTOR
        self.fat = [0] * (geo["clusters"] + 2)
        self.fat[0] = 0x0FFFFFF8
        self.fat[1] = 0x0FFFFFFF
        self.next = 2
        self.root = self.alloc()

    @property
    def cbytes(self):
        return self.g["spc"] * SECTOR

    def data_off(self, cluster):
        g = self.g
        sector = g["reserved"] + g["nfats"] * g["fat_size"] + (cluster - 2) * g["spc"]
        return self.part0 + sector * SECTOR

    def alloc(self):
        c = self.next
        self.next += 1
        self.fat[c] = 0x0FFFFFFF
        return c

    def write_chain(self, data):
        if not data:
            return 0
        first = prev = 0
        for i in range(0, len(data), self.cbytes):
            c = self.alloc()
            if prev:
                self.fat[prev] = c
            else:
                first = c
            prev = c
            chunk = data[i:i + self.cbytes]
            off = self.data_off(c)
            self.img[off:off + len(chunk)] = chunk
        return first

    @staticmethod
    def checksum(sfn):
        s = 0
        for b in sfn:
            s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
        return s

    def entries_for(self, name, sfn, attr, cluster, size):
        out = []
        need_lfn = (sfn.decode("ascii").rstrip().replace(" ", "") != name.upper().replace(".", "")
                    or name != name.upper())
        if need_lfn:
            chars = [ord(c) for c in name] + [0]
            while len(chars) % 13:
                chars.append(0xFFFF)
            pieces = len(chars) // 13
            chk = self.checksum(sfn)
            for p in range(pieces, 0, -1):
                part = chars[(p - 1) * 13:p * 13]
                e = bytearray(32)
                e[0] = p | (0x40 if p == pieces else 0)
                e[11] = 0x0F
                e[13] = chk
                for k, off in enumerate([1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]):
                    struct.pack_into("<H", e, off, part[k])
                out.append(bytes(e))
        e = bytearray(32)
        e[0:11] = sfn
        e[11] = attr
        struct.pack_into("<HHHHHHHI", e, 14, 0x6000, 0x5A21, 0x5A21, cluster >> 16, 0x6000, 0x5A21,
                         cluster & 0xFFFF, size)
        out.append(bytes(e))
        return out

    def add_dir(self, entries, cluster_hint=None):
        raw = b"".join(entries)
        c = cluster_hint or self.alloc()
        # directories here are small: they fit in a few clusters
        first = c
        prev = c
        for i in range(0, max(len(raw), 1), self.cbytes):
            if i > 0:
                n = self.alloc()
                self.fat[prev] = n
                prev = n
                c = n
            chunk = raw[i:i + self.cbytes]
            off = self.data_off(c)
            self.img[off:off + len(chunk)] = chunk
        return first

    def finish(self, volume_label):
        g = self.g
        bs = bytearray(SECTOR)
        bs[0:3] = b"\xEB\x58\x90"
        bs[3:11] = b"WINCLONE"
        struct.pack_into("<HBHBHHBHHHII", bs, 11, SECTOR, g["spc"], g["reserved"], g["nfats"], 0, 0,
                         0xF8, 0, 63, 255, PART_START, g["part_sectors"])
        struct.pack_into("<IHHIHH", bs, 36, g["fat_size"], 0, 0, self.root, 1, 6)
        bs[64] = 0x80
        bs[66] = 0x29
        struct.pack_into("<I", bs, 67, 0x57434C4E)
        bs[71:82] = volume_label.ljust(11).encode("ascii")
        bs[82:90] = b"FAT32   "
        bs[510:512] = b"\x55\xAA"
        fsinfo = bytearray(SECTOR)
        struct.pack_into("<I", fsinfo, 0, 0x41615252)
        struct.pack_into("<I", fsinfo, 484, 0x61417272)
        struct.pack_into("<II", fsinfo, 488, g["clusters"] - self.next, self.next)
        struct.pack_into("<I", fsinfo, 508, 0xAA550000)
        for sector, blob in ((0, bs), (1, fsinfo), (6, bs), (7, fsinfo)):
            off = self.part0 + sector * SECTOR
            self.img[off:off + SECTOR] = blob
        fat_bytes = b"".join(struct.pack("<I", v) for v in self.fat)
        fat_bytes = fat_bytes.ljust(g["fat_size"] * SECTOR, b"\x00")
        for i in range(g["nfats"]):
            off = self.part0 + (g["reserved"] + i * g["fat_size"]) * SECTOR
            self.img[off:off + len(fat_bytes)] = fat_bytes
        # MBR with a single FAT32 (LBA) partition
        mbr = bytearray(SECTOR)
        mbr[446] = 0x80
        mbr[446 + 1:446 + 4] = b"\x00\x02\x00"
        mbr[446 + 4] = 0x0C
        mbr[446 + 5:446 + 8] = b"\xFE\xFF\xFF"
        struct.pack_into("<II", mbr, 446 + 8, PART_START, g["part_sectors"])
        mbr[510:512] = b"\x55\xAA"
        self.img[0:SECTOR] = mbr
        return bytes(self.img)


def sfn_of(base, ext=""):
    return (base.upper().ljust(8) + ext.upper().ljust(3)).encode("ascii")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    out = args[0] if args else os.path.join("out", "run", "data.img")
    size_mb = 128
    if "--size-mb" in sys.argv:
        size_mb = int(sys.argv[sys.argv.index("--size-mb") + 1])
        args = [a for a in args if a != str(size_mb)]
        out = args[0] if args else out
    if os.path.exists(out) and "--force" not in sys.argv:
        print("%s exists, keeping it (use --force to recreate)" % out)
        return
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)

    fs = Fat32(build(size_mb))
    readme = (b"Welcome to WinClone.\r\nThis file lives on a FAT32 partition and was read by the\r\n"
              b"kernel's own AHCI and FAT32 drivers.\r\n")
    hello = b"Hello from a sub-directory.\r\n"
    long_text = b"This file has a long name stored in VFAT long-name records.\r\n"

    docs_cluster = fs.alloc()
    docs_entries = [
        # "." and ".." for the directory
        bytes(fs.entries_for(".", b".".ljust(11), 0x10, docs_cluster, 0)[-1]),
        bytes(fs.entries_for("..", b"..".ljust(11), 0x10, 0, 0)[-1]),
    ]
    docs_entries += fs.entries_for("HELLO.TXT", sfn_of("HELLO", "TXT"), 0x20, fs.write_chain(hello), len(hello))
    docs_entries += fs.entries_for("Long file name example.txt", sfn_of("LONGFI~1", "TXT"), 0x20,
                                   fs.write_chain(long_text), len(long_text))
    fs.add_dir(docs_entries, docs_cluster)

    root_entries = []
    label = bytearray(32)
    label[0:11] = b"WINCLONE   "
    label[11] = 0x08
    root_entries.append(bytes(label))
    root_entries += fs.entries_for("README.TXT", sfn_of("README", "TXT"), 0x20, fs.write_chain(readme), len(readme))
    root_entries += fs.entries_for("DOCS", sfn_of("DOCS"), 0x10, docs_cluster, 0)
    fs.add_dir(root_entries, fs.root)

    with open(out, "wb") as f:
        f.write(fs.finish("WINCLONE"))
    print("created %s (%d MiB, FAT32, %d sectors/cluster)" % (out, size_mb, fs.g["spc"]))


if __name__ == "__main__":
    main()
