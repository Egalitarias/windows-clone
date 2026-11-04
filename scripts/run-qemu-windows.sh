#!/usr/bin/env bash
# Boot out/uefi in QEMU on Windows (Git Bash) using the edk2 firmware that
# ships with the Scoop/QEMU install. Run `make CC=clang uefi-loader` first.
# Usage: scripts/run-qemu-windows.sh [--window] [seconds-timeout]
set -euo pipefail

cd "$(dirname "$0")/.."
export PATH="$HOME/scoop/shims:$HOME/scoop/apps/qemu/current:$PATH"

DISPLAY_MODE=none
if [ "${1:-}" = "--window" ]; then DISPLAY_MODE=default; shift; fi
TIMEOUT="${1:-30}"

QEMU_SHARE="${QEMU_SHARE:-$HOME/scoop/apps/qemu/current/share}"
CODE="$QEMU_SHARE/edk2-x86_64-code.fd"
VARS_TEMPLATE="$QEMU_SHARE/edk2-i386-vars.fd"

[ -f out/uefi/EFI/BOOT/BOOTX64.EFI ] || { echo "Run: make CC=clang uefi-loader" >&2; exit 1; }
[ -f "$CODE" ] || { echo "Missing $CODE (set QEMU_SHARE)" >&2; exit 1; }

mkdir -p out/run
cp "$VARS_TEMPLATE" out/run/vars.fd

timeout "$TIMEOUT" qemu-system-x86_64 \
  -machine q35 -m 512 -display "$DISPLAY_MODE" \
  -serial stdio -monitor none \
  -drive if=pflash,format=raw,readonly=on,file="$CODE" \
  -drive if=pflash,format=raw,file=out/run/vars.fd \
  -drive format=raw,file=fat:rw:out/uefi || [ $? -eq 124 ]
