#!/bin/bash

# Verify boot and system state from serial output
# Usage: verify-boot.sh <log_file> <stage> [timeout_seconds]

set -e

LOG_FILE="${1:-}"
STAGE="${2:-boot}"
TIMEOUT="${3:-30}"

if [[ -z "$LOG_FILE" ]]; then
    echo "Usage: $0 <log_file> <stage> [timeout_seconds]"
    echo "Stages: boot, uefi, installer, system"
    exit 1
fi

if [[ ! -f "$LOG_FILE" ]]; then
    echo "Error: Log file not found: $LOG_FILE"
    exit 1
fi

# Verify based on stage
case "$STAGE" in
    boot)
        # Verify UEFI boot and kernel entry
        if grep -q "UEFI.*boot" "$LOG_FILE" 2>/dev/null || grep -q "KERNEL_BOOT_INFO" "$LOG_FILE" 2>/dev/null; then
            echo "✓ UEFI boot detected"
            return 0
        fi
        echo "✗ UEFI boot not detected"
        return 1
        ;;
    uefi)
        # Verify UEFI console and memory initialization
        if grep -q "ConOut\|memory map\|PMM initialized" "$LOG_FILE" 2>/dev/null; then
            echo "✓ UEFI console initialized"
            return 0
        fi
        echo "✗ UEFI initialization incomplete"
        return 1
        ;;
    installer)
        # Verify installer completed
        if grep -q "install.sh\|EFI.*copied\|installer complete" "$LOG_FILE" 2>/dev/null; then
            echo "✓ Installer completed"
            return 0
        fi
        echo "✗ Installer did not complete"
        return 1
        ;;
    system)
        # Verify kernel scheduler running
        if grep -q "Scheduler running\|Task.*running\|Context switch\|Tests passed" "$LOG_FILE" 2>/dev/null; then
            echo "✓ System kernel running"
            return 0
        fi
        echo "✗ Kernel not running"
        return 1
        ;;
    *)
        echo "Unknown stage: $STAGE"
        exit 1
        ;;
esac
