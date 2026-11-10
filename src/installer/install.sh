#!/usr/bin/env bash
#
# WinClone Installer (UEFI-standard deploy)
# Installs BOOTX64.EFI and KERNEL.BIN into <ESP>/EFI/BOOT.
#

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1" >&2
}

usage() {
    cat <<'EOF'
Usage:
  install.sh [options]

Options:
  --source-dir <path>      Source directory containing BOOTX64.EFI/KERNEL.BIN
                           or EFI/BOOT/BOOTX64.EFI + KERNEL.BIN, and optionally
                           an APPS folder with the programs (copied to <ESP>/APPS)
                           (default: current directory)
  --esp-mount <path>       Mountpoint of EFI System Partition (default: /boot/efi)
  --esp-device <device>    Block device to mount as ESP if --esp-mount is not mounted
  --force                  Replace existing target files without warning
  --dry-run                Validate and print actions without modifying disk
  --help                   Show this help

Examples:
  sudo ./install.sh --source-dir out/installer-package
  sudo ./install.sh --source-dir out/uefi --esp-device /dev/nvme0n1p1
EOF
}

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || {
        log_error "Required command not found: $1"
        exit 1
    }
}

is_mounted() {
    local mount_path="$1"
    if command -v mountpoint >/dev/null 2>&1; then
        mountpoint -q "$mount_path"
        return
    fi
    grep -qs " $mount_path " /proc/mounts
}

resolve_source_file() {
    local source_dir="$1"
    local filename="$2"
    local direct="$source_dir/$filename"
    local nested="$source_dir/EFI/BOOT/$filename"

    if [ -f "$direct" ]; then
        printf "%s" "$direct"
        return 0
    fi
    if [ -f "$nested" ]; then
        printf "%s" "$nested"
        return 0
    fi

    return 1
}

SOURCE_DIR="."
ESP_MOUNT="/boot/efi"
ESP_DEVICE=""
FORCE=0
DRY_RUN=0
MOUNTED_BY_SCRIPT=0

cleanup() {
    if [ "$MOUNTED_BY_SCRIPT" -eq 1 ] && is_mounted "$ESP_MOUNT"; then
        umount "$ESP_MOUNT" && log_info "Unmounted $ESP_MOUNT (cleanup)"
        MOUNTED_BY_SCRIPT=0
    fi
}
trap cleanup EXIT INT TERM

while [ "$#" -gt 0 ]; do
    case "$1" in
        --source-dir)
            [ -n "${2:-}" ] || { log_error "--source-dir requires a path argument"; usage; exit 1; }
            SOURCE_DIR="$2"
            shift 2
            ;;
        --esp-mount)
            [ -n "${2:-}" ] || { log_error "--esp-mount requires a path argument"; usage; exit 1; }
            ESP_MOUNT="$2"
            shift 2
            ;;
        --esp-device)
            [ -n "${2:-}" ] || { log_error "--esp-device requires a device argument"; usage; exit 1; }
            ESP_DEVICE="$2"
            shift 2
            ;;
        --force)
            FORCE=1
            shift
            ;;
        --dry-run)
            DRY_RUN=1
            shift
            ;;
        --help)
            usage
            exit 0
            ;;
        *)
            log_error "Unknown option: $1"
            usage
            exit 1
            ;;
    esac
done

require_cmd cp
require_cmd mkdir
require_cmd sync
require_cmd cmp
require_cmd mount
require_cmd umount

if [ "$(id -u)" -ne 0 ]; then
    log_error "Installer must run as root."
    exit 1
fi

if [ ! -d "$SOURCE_DIR" ]; then
    log_error "Source directory does not exist: $SOURCE_DIR"
    exit 1
fi

BOOTLOADER_SOURCE="$(resolve_source_file "$SOURCE_DIR" "BOOTX64.EFI" || true)"
KERNEL_SOURCE="$(resolve_source_file "$SOURCE_DIR" "KERNEL.BIN" || true)"
if [ -z "$BOOTLOADER_SOURCE" ]; then
    log_error "BOOTX64.EFI not found in $SOURCE_DIR or $SOURCE_DIR/EFI/BOOT"
    exit 1
fi
if [ -z "$KERNEL_SOURCE" ]; then
    log_error "KERNEL.BIN not found in $SOURCE_DIR or $SOURCE_DIR/EFI/BOOT"
    exit 1
fi

TARGET_BOOT_DIR="$ESP_MOUNT/EFI/BOOT"
TARGET_BOOTLOADER="$TARGET_BOOT_DIR/BOOTX64.EFI"
TARGET_KERNEL="$TARGET_BOOT_DIR/KERNEL.BIN"
TIMESTAMP="$(date +%Y%m%d-%H%M%S)"

log_info "Source BOOTX64.EFI: $BOOTLOADER_SOURCE"
log_info "Source KERNEL.BIN:   $KERNEL_SOURCE"
log_info "Target ESP mount:    $ESP_MOUNT"
log_info "Target path:         $TARGET_BOOT_DIR"

if ! is_mounted "$ESP_MOUNT"; then
    if [ -z "$ESP_DEVICE" ]; then
        log_error "ESP mountpoint is not mounted: $ESP_MOUNT"
        log_error "Provide --esp-device <device> or mount the ESP first."
        exit 1
    fi

    if [ "$DRY_RUN" -eq 1 ]; then
        log_info "[dry-run] Would mount $ESP_DEVICE at $ESP_MOUNT"
    else
        mkdir -p "$ESP_MOUNT"
        mount -t vfat "$ESP_DEVICE" "$ESP_MOUNT"
        MOUNTED_BY_SCRIPT=1
        log_info "Mounted ESP device $ESP_DEVICE at $ESP_MOUNT"
    fi
fi

if [ "$DRY_RUN" -eq 1 ]; then
    log_info "[dry-run] Would create target directory: $TARGET_BOOT_DIR"
else
    mkdir -p "$TARGET_BOOT_DIR"
fi

if [ "$FORCE" -ne 1 ]; then
    if [ -e "$TARGET_BOOTLOADER" ] || [ -e "$TARGET_KERNEL" ]; then
        log_warn "Existing files found under $TARGET_BOOT_DIR."
        log_warn "Re-run with --force to replace them."
        exit 1
    fi
fi

if [ "$DRY_RUN" -eq 1 ]; then
    log_info "[dry-run] Would copy $BOOTLOADER_SOURCE -> $TARGET_BOOTLOADER"
    log_info "[dry-run] Would copy $KERNEL_SOURCE -> $TARGET_KERNEL"
else
    if [ -f "$TARGET_BOOTLOADER" ]; then
        cp "$TARGET_BOOTLOADER" "$TARGET_BOOTLOADER.bak.$TIMESTAMP"
        log_info "Backed up existing BOOTX64.EFI"
    fi
    if [ -f "$TARGET_KERNEL" ]; then
        cp "$TARGET_KERNEL" "$TARGET_KERNEL.bak.$TIMESTAMP"
        log_info "Backed up existing KERNEL.BIN"
    fi

    cp "$BOOTLOADER_SOURCE" "$TARGET_BOOTLOADER"
    cp "$KERNEL_SOURCE" "$TARGET_KERNEL"
    sync

    cmp -s "$BOOTLOADER_SOURCE" "$TARGET_BOOTLOADER" || {
        log_error "Verification failed for BOOTX64.EFI copy"
        exit 1
    }
    cmp -s "$KERNEL_SOURCE" "$TARGET_KERNEL" || {
        log_error "Verification failed for KERNEL.BIN copy"
        exit 1
    }

    log_info "Installed BOOTX64.EFI and KERNEL.BIN to $TARGET_BOOT_DIR"
fi

# The programs (Notes, Files, Paint, ...) live in <ESP>/APPS; the kernel loads them from there.
APPS_SOURCE="$SOURCE_DIR/APPS"
TARGET_APPS="$ESP_MOUNT/APPS"
if [ -d "$APPS_SOURCE" ]; then
    if [ "$DRY_RUN" -eq 1 ]; then
        log_info "[dry-run] Would copy $APPS_SOURCE/*.BIN -> $TARGET_APPS"
    else
        mkdir -p "$TARGET_APPS"
        for program in "$APPS_SOURCE"/*.BIN; do
            [ -f "$program" ] || continue
            cp "$program" "$TARGET_APPS/"
        done
        sync
        for program in "$APPS_SOURCE"/*.BIN; do
            [ -f "$program" ] || continue
            cmp -s "$program" "$TARGET_APPS/$(basename "$program")" || {
                log_error "Verification failed for $(basename "$program")"
                exit 1
            }
        done
        log_info "Installed programs to $TARGET_APPS"
    fi
else
    log_warn "No APPS folder in $SOURCE_DIR: Notes, Files and the other programs will not be installed."
fi

if [ "$MOUNTED_BY_SCRIPT" -eq 1 ]; then
    if [ "$DRY_RUN" -eq 1 ]; then
        log_info "[dry-run] Would unmount $ESP_MOUNT"
    elif is_mounted "$ESP_MOUNT"; then
        umount "$ESP_MOUNT"
        MOUNTED_BY_SCRIPT=0
        log_info "Unmounted $ESP_MOUNT"
    fi
fi

if [ "$DRY_RUN" -eq 1 ]; then
    log_info "Dry run complete."
else
    log_info "Install complete."
fi
