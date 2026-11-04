#!/usr/bin/env python3
"""Independent FAT32 reader and checker for disk images (verifies what the OS wrote).

Usage:
    python scripts/fat32-ls.py IMAGE --gpt      show and validate the GPT (CRCs, backup, overlaps)
    python scripts/fat32-ls.py IMAGE [--part N] list the whole tree (N = GPT/MBR partition number)
    python scripts/fat32-ls.py IMAGE --cat PATH print a file (PATH like /DOCS/HELLO.TXT)
    python scripts/fat32-ls.py IMAGE --check    consistency check (exit status 1 on problems)

Handles an MBR with FAT32 partitions, a GPT disk, or a bare FAT32 volume. This is a
deliberately separate implementation from the kernel's driver. Standard
library only.
"""
import struct
import sys
import zlib

SECTOR = 512


def parse_gpt(f):
    """Return (header, entries, problems). Validates both headers and entry arrays."""
    def read(lba, n=1):
        f.seek(lba * SECTOR)
        return f.read(n * SECTOR)

    problems = []
    f.seek(0, 2)
    total = f.tell() // SECTOR
    mbr = read(0)
    if mbr[510:512] != b"\x55\xAA" or mbr[446 + 4] != 0xEE:
        return None, [], ["no protective MBR"]
    result = None
    for name, lba in (("primary", 1), ("backup", total - 1)):
        h = read(lba)
        if h[0:8] != b"EFI PART":
            problems.append("%s header: bad signature" % name)
            continue
        size = struct.unpack_from("<I", h, 12)[0]
        crc = struct.unpack_from("<I", h, 16)[0]
        body = bytearray(h[:size])
        body[16:20] = b"\x00\x00\x00\x00"
        if zlib.crc32(bytes(body)) != crc:
            problems.append("%s header: CRC mismatch" % name)
        my, alt, first, last = struct.unpack_from("<QQQQ", h, 24)
        guid = h[56:72]
        ent_lba, n, esize, ecrc = struct.unpack_from("<QIII", h, 72)
        if my != lba:
            problems.append("%s header: MyLBA %d != %d" % (name, my, lba))
        entries = read(ent_lba, (n * esize + SECTOR - 1) // SECTOR)[:n * esize]
        if zlib.crc32(entries) != ecrc:
            problems.append("%s entry array: CRC mismatch" % name)
        parsed = []
        for i in range(n):
            e = entries[i * esize:(i + 1) * esize]
            if e[0:16] == b"\x00" * 16:
                continue
            s, en = struct.unpack_from("<QQ", e, 32)
            label = e[56:128].decode("utf-16-le").split("\x00")[0]
            if s < first or en > last or en < s:
                problems.append("%s: partition %d outside usable area" % (name, i + 1))
            parsed.append(dict(index=i + 1, type=e[0:16].hex(), start=s, end=en, name=label))
        if result is None:
            result = dict(first=first, last=last, guid=guid.hex(), total=total), parsed
        elif result[1] != parsed:
            problems.append("primary and backup partition entries differ")
    for i, a in enumerate(result[1] if result else []):
        for b in (result[1] if result else [])[i + 1:]:
            if a["start"] <= b["end"] and b["start"] <= a["end"]:
                problems.append("partitions %d and %d overlap" % (a["index"], b["index"]))
    return (result[0] if result else None), (result[1] if result else []), problems


class Volume:
    def __init__(self, path, part=None):
        self.f = open(path, "rb")
        self.base = 0
        boot = self.read(0, SECTOR)
        if part is not None or not self.valid_boot(boot):
            header, entries, _ = parse_gpt(self.f)
            if header is not None:
                chosen = [e for e in entries if part is None or e["index"] == part]
                for e in chosen:
                    self.base = e["start"] * SECTOR
                    if self.valid_boot(self.read(0, SECTOR)):
                        break
                boot = self.read(0, SECTOR)
        if not self.valid_boot(boot):
            # try MBR partitions
            for i in range(4):
                if part is not None and i + 1 != part:
                    continue
                e = boot[446 + i * 16:446 + (i + 1) * 16] if self.base == 0 else b"\x00" * 16
                if e[4] in (0x0B, 0x0C):
                    self.base = struct.unpack_from("<I", e, 8)[0] * SECTOR
                    boot = self.read(0, SECTOR)
                    break
        if not self.valid_boot(boot):
            raise SystemExit("no FAT32 volume found")
        (self.bps, self.spc, self.reserved, self.nfats) = struct.unpack_from("<HBHB", boot, 11)
        self.fat_size = struct.unpack_from("<I", boot, 36)[0]
        self.root = struct.unpack_from("<I", boot, 44)[0]
        total = struct.unpack_from("<H", boot, 19)[0] or struct.unpack_from("<I", boot, 32)[0]
        self.total = total
        self.data_start = self.reserved + self.nfats * self.fat_size
        self.clusters = (total - self.data_start) // self.spc
        self.label = boot[71:82].decode("ascii", "replace").strip()
        self.fats = []
        for i in range(self.nfats):
            raw = self.read((self.reserved + i * self.fat_size) * SECTOR, self.fat_size * SECTOR)
            self.fats.append(struct.unpack("<%dI" % (len(raw) // 4), raw))

    @staticmethod
    def valid_boot(b):
        if b[510:512] != b"\x55\xAA":
            return False
        bps, spc = struct.unpack_from("<HB", b, 11)
        fat32 = struct.unpack_from("<I", b, 36)[0]
        return bps == 512 and spc and (spc & (spc - 1)) == 0 and fat32 != 0

    def read(self, offset, n):
        self.f.seek(self.base + offset)
        return self.f.read(n)

    def chain(self, cluster):
        out = []
        seen = set()
        while 2 <= cluster < self.clusters + 2 and cluster not in seen:
            seen.add(cluster)
            out.append(cluster)
            cluster = self.fats[0][cluster] & 0x0FFFFFFF
            if cluster >= 0x0FFFFFF8:
                break
        return out

    def read_chain(self, cluster, size=None):
        data = b""
        for c in self.chain(cluster):
            off = (self.data_start + (c - 2) * self.spc) * SECTOR
            data += self.read(off, self.spc * SECTOR)
        return data if size is None else data[:size]

    def entries(self, cluster):
        """Yield dicts for the visible entries of a directory."""
        raw = self.read_chain(cluster)
        lfn = {}
        for i in range(0, len(raw), 32):
            e = raw[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5:
                lfn = {}
                continue
            if e[11] & 0x3F == 0x0F:
                seq = e[0] & 0x1F
                chars = b"".join(e[o:o + n] for o, n in ((1, 10), (14, 12), (28, 4)))
                text = chars.decode("utf-16-le")
                text = text.split("\x00")[0].rstrip("￿")
                lfn[seq] = (text, e[13])
                continue
            if e[11] & 0x08:
                lfn = {}
                continue
            sfn = bytes(e[0:11])
            chk = 0
            for b in sfn:
                chk = (((chk & 1) << 7) + (chk >> 1) + b) & 0xFF
            if lfn and all(v[1] == chk for v in lfn.values()):
                name = "".join(lfn[k][0] for k in sorted(lfn))
            else:
                base = sfn[:8].decode("ascii", "replace").rstrip()
                ext = sfn[8:].decode("ascii", "replace").rstrip()
                name = base + ("." + ext if ext else "")
            lfn = {}
            if name in (".", ".."):
                continue
            hi, lo = struct.unpack_from("<H", e, 20)[0], struct.unpack_from("<H", e, 26)[0]
            yield dict(name=name, attr=e[11], cluster=(hi << 16) | lo,
                       size=struct.unpack_from("<I", e, 28)[0], sfn=sfn)


def walk(vol, cluster, path, callback, depth=0):
    for ent in vol.entries(cluster):
        full = path + "/" + ent["name"]
        callback(ent, full, depth)
        if ent["attr"] & 0x10:
            walk(vol, ent["cluster"], full, callback, depth + 1)


def resolve(vol, path):
    cluster = vol.root
    ent = None
    for part in [p for p in path.split("/") if p]:
        for cand in vol.entries(cluster):
            if cand["name"].lower() == part.lower():
                ent = cand
                cluster = cand["cluster"]
                break
        else:
            raise SystemExit("not found: " + path)
    return ent


def check(vol):
    problems = []
    if vol.fats[0] != vol.fats[1] if vol.nfats > 1 else False:
        problems.append("FAT copies differ")
    used = set()
    seen_items = []

    def visit(ent, full, depth):
        seen_items.append(full)
        if ent["attr"] & 0x10 or ent["size"]:
            ch = vol.chain(ent["cluster"]) if ent["cluster"] else []
            if ent["size"] and ent["cluster"] == 0:
                problems.append("%s: size %d but no cluster" % (full, ent["size"]))
            need = -(-ent["size"] // (vol.spc * SECTOR)) if not ent["attr"] & 0x10 else len(ch)
            if not ent["attr"] & 0x10 and len(ch) != need:
                problems.append("%s: chain has %d clusters, size needs %d" % (full, len(ch), need))
            for c in ch:
                if c in used:
                    problems.append("%s: cluster %d is cross-linked" % (full, c))
                used.add(c)

    walk(vol, vol.root, "", visit)
    for c in vol.chain(vol.root):
        used.add(c)
    allocated = {c for c in range(2, vol.clusters + 2) if vol.fats[0][c] & 0x0FFFFFFF}
    leaked = allocated - used
    if leaked:
        problems.append("%d allocated clusters are not reachable (leak), e.g. %s" %
                        (len(leaked), sorted(leaked)[:5]))
    return problems, len(seen_items)


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    if "--gpt" in sys.argv:
        with open(sys.argv[1], "rb") as f:
            header, entries, problems = parse_gpt(f)
        if header:
            print("disk GUID %s, usable LBA %d-%d of %d" % (header["guid"], header["first"],
                                                         header["last"], header["total"]))
        for e in entries:
            print("  partition %d: LBA %d-%d (%d MiB) type %s name %r" % (
                e["index"], e["start"], e["end"], (e["end"] - e["start"] + 1) // 2048, e["type"], e["name"]))
        for p in problems:
            print("PROBLEM:", p)
        print("GPT: %s" % ("OK" if not problems else "%d problem(s)" % len(problems)))
        sys.exit(1 if problems else 0)
    part = int(sys.argv[sys.argv.index("--part") + 1]) if "--part" in sys.argv else None
    vol = Volume(sys.argv[1], part)
    if "--cat" in sys.argv:
        path = sys.argv[sys.argv.index("--cat") + 1]
        ent = resolve(vol, path)
        sys.stdout.buffer.write(vol.read_chain(ent["cluster"], ent["size"]))
        return
    if "--check" in sys.argv:
        problems, items = check(vol)
        for p in problems:
            print("PROBLEM:", p)
        print("volume %r: %d files/dirs checked, %s" % (
            vol.label, items, "OK" if not problems else "%d problem(s)" % len(problems)))
        sys.exit(1 if problems else 0)
    print("Volume label: %s   clusters: %d x %d bytes" % (vol.label, vol.clusters, vol.spc * SECTOR))

    def show(ent, full, depth):
        kind = "<DIR>" if ent["attr"] & 0x10 else "%7d" % ent["size"]
        print("%s%-40s %s" % ("  " * depth, ent["name"], kind))

    walk(vol, vol.root, "", show)


if __name__ == "__main__":
    main()
