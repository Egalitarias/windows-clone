#!/bin/sh
# Make an exFAT test image holding the files the kernel's exFAT self-test expects
# (build the kernel with `make CC=clang FS_SELFTEST=1 uefi-loader`, attach the image as a data disk).
#
# Needs Linux with exfatprogs (mkfs.exfat, fsck.exfat) and loop mounts, as root. On Windows use WSL:
#   wsl -u root -e sh scripts/make-exfat-image.sh out/run/ex.img 64M
#   wsl -u root -e sh scripts/make-exfat-image.sh out/run/ex32k.img 128M "-c 32768"
#
# Usage: make-exfat-image.sh [image] [size] [mkfs.exfat options]
set -e
cd "$(dirname "$0")/.."
OUT=${1:-out/run/ex.img}
SIZE=${2:-64M}
OPTS=${3:-}
mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
truncate -s "$SIZE" "$OUT"
mkfs.exfat $OPTS -L EXTEST "$OUT" > /dev/null
MNT=$(mktemp -d)
mount -o loop "$OUT" "$MNT"
python3 - "$MNT" <<'PY'
import os, sys
os.chdir(sys.argv[1])
open('PRE.BIN', 'wb').write(bytes(((i * 13 + i // 97) & 0xFF) for i in range(3000000)))
open('HELLO.TXT', 'w').write('Hello from Linux\n')
open('A fairly long file name that needs several name entries.txt', 'w').write('long\n')
os.mkdir('PREDIR')
for i in range(1, 201):
    open('PREDIR/f%d.txt' % i, 'wb').write(bytes((j % 251) for j in range(i)))
PY
sync
umount "$MNT"
rmdir "$MNT"
fsck.exfat "$OUT"
echo "wrote $OUT"
