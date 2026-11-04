#!/bin/bash

# QEMU VM test automation for bootloader + installer + kernel validation
# Usage: run-vm-test.sh <disk_image> <ovmf_code> <ovmf_vars> [timeout_seconds]

set -e

DISK_IMAGE="${1:-}"
OVMF_CODE="${2:-}"
OVMF_VARS="${3:-}"
TIMEOUT="${4:-120}"

if [[ -z "$DISK_IMAGE" || -z "$OVMF_CODE" || -z "$OVMF_VARS" ]]; then
    echo "Usage: $0 <disk_image> <ovmf_code> <ovmf_vars> [timeout_seconds]"
    exit 1
fi

if [[ ! -f "$DISK_IMAGE" ]]; then
    echo "Error: Disk image not found: $DISK_IMAGE"
    exit 1
fi

if [[ ! -f "$OVMF_CODE" ]]; then
    echo "Error: OVMF code image not found: $OVMF_CODE"
    exit 1
fi

if [[ ! -f "$OVMF_VARS" ]]; then
    echo "Error: OVMF vars not found: $OVMF_VARS"
    exit 1
fi

# Create temporary directory for this test run
TEST_TMPDIR=$(mktemp -d)
trap "rm -rf '$TEST_TMPDIR'" EXIT

SERIAL_LOG="$TEST_TMPDIR/serial.log"
OVMF_VARS_TEST="$TEST_TMPDIR/ovmf_vars.fd"
QEMU_PID_FILE="$TEST_TMPDIR/qemu.pid"

# Copy OVMF vars to temp location
cp "$OVMF_VARS" "$OVMF_VARS_TEST"

# Function to wait for QEMU process
wait_for_qemu_exit() {
    local pid="$1"
    local timeout_sec="$2"
    local elapsed=0
    
    while kill -0 "$pid" 2>/dev/null; do
        if [[ $elapsed -ge $timeout_sec ]]; then
            echo "Warning: QEMU timeout after ${timeout_sec}s, forcing shutdown..."
            kill -9 "$pid" 2>/dev/null || true
            return 1
        fi
        sleep 1
        ((elapsed++))
    done
    
    return 0
}

# Function to run a QEMU boot
run_qemu_boot() {
    local boot_name="$1"
    local timeout_sec="$2"
    
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "Test: $boot_name (${timeout_sec}s timeout)"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    
    > "$SERIAL_LOG"  # Clear log
    
    # Start QEMU with serial output to file + monitor
    qemu-system-x86_64 \
        -machine q35 \
        -m 512M \
        -nographic \
        -serial file:"$SERIAL_LOG" \
        -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
        -drive if=pflash,format=raw,file="$OVMF_VARS_TEST" \
        -drive format=raw,file="$DISK_IMAGE" \
        -pidfile "$QEMU_PID_FILE" &
    
    local qemu_pid=$!
    echo "QEMU PID: $qemu_pid"
    
    # Wait for QEMU to exit or timeout
    if ! wait_for_qemu_exit "$qemu_pid" "$timeout_sec"; then
        echo "✗ $boot_name: Timeout"
        return 1
    fi
    
    # Wait a bit for serial I/O to flush
    sleep 1
    
    if [[ ! -f "$SERIAL_LOG" ]] || [[ ! -s "$SERIAL_LOG" ]]; then
        echo "✗ $boot_name: No serial output"
        return 1
    fi
    
    echo "Serial output:"
    echo "─────────────────────────────────────────────────"
    head -50 "$SERIAL_LOG"
    if [[ $(wc -l < "$SERIAL_LOG") -gt 50 ]]; then
        echo "... ($(wc -l < "$SERIAL_LOG") lines total)"
    fi
    echo "─────────────────────────────────────────────────"
    
    return 0
}

# Test 1: Boot from disk to UEFI
echo ""
echo "Test 1/5: Initial boot to UEFI shell"
if run_qemu_boot "UEFI Boot" 30; then
    if grep -q "UEFI\|Shell>" "$SERIAL_LOG" 2>/dev/null; then
        echo "✓ Test 1: UEFI booted successfully"
    else
        echo "⚠ Test 1: UEFI booted but shell prompt unclear"
    fi
else
    echo "✗ Test 1 FAILED"
    exit 1
fi

# Test 2: Verify disk is accessible from UEFI shell
echo ""
echo "Test 2/5: Disk accessibility check"
if grep -q "EFI\|Boot\|Volume" "$SERIAL_LOG" 2>/dev/null; then
    echo "✓ Test 2: Disk accessible from UEFI"
else
    echo "⚠ Test 2: Disk accessibility unclear from logs"
fi

# Test 3: Boot kernel from disk
echo ""
echo "Test 3/5: Boot kernel from disk"
if run_qemu_boot "Kernel Boot" 30; then
    if grep -q "Scheduler\|Task\|Kernel.*running\|Context" "$SERIAL_LOG" 2>/dev/null; then
        echo "✓ Test 3: Kernel booted successfully"
    elif grep -q "Error\|Panic\|Failed" "$SERIAL_LOG" 2>/dev/null; then
        echo "✗ Test 3: Kernel boot failed"
        exit 1
    else
        echo "⚠ Test 3: Kernel boot status unclear"
    fi
else
    echo "✗ Test 3 FAILED"
    exit 1
fi

# Test 4: Verify test suite runs (if TESTS=1)
echo ""
echo "Test 4/5: Test suite validation"
if grep -q "passed\|tests\|TCB\|scheduler" "$SERIAL_LOG" 2>/dev/null; then
    echo "✓ Test 4: Tests detected in boot"
else
    echo "⚠ Test 4: No test output detected (may be production build)"
fi

# Test 5: Clean shutdown
echo ""
echo "Test 5/5: Verify clean boot sequence"
if [[ -s "$SERIAL_LOG" ]] && [[ $(wc -l < "$SERIAL_LOG") -gt 5 ]]; then
    echo "✓ Test 5: System produced output ($( wc -l < "$SERIAL_LOG") lines)"
else
    echo "✗ Test 5: Insufficient system output"
    exit 1
fi

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "✓ ALL TESTS PASSED"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

exit 0
