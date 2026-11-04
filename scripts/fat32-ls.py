#!/usr/bin/env python3
"""Independent FAT32 reader and checker for disk images (verifies what the OS wrote).

Usage:
    python scripts/fat32-ls.py IMAGE            list the whole tree
    python scripts/fat32-ls.py IMAGE --cat PATH print a file (PATH like /DOCS/HELLO.TXT)
    python scripts/fat32-ls.py IMAGE --check    consistency check (exit status 1 on problems)

Handles an MBR with one FAT32 partition, or a bare FAT32 volume. This is a
deliberately separate implementation from the kernel's driver. Standard
library only.
"""
import struct
import sys

SECTOR = 512


class Volume:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.base = 0
        boot = self.read(0, SECTOR)
        if not self.valid_boot(boot):
            # try MBR partitions
            for i in range(4):
                e = boot[446 + i * 16:446 + (i + 1) * 16]
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
    vol = Volume(sys.argv[1])
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
