#!/bin/bash

# Comprehensive VM test suite runner
# Performs: disk creation → boot → kernel validation → reporting
# Usage: test-vm-full.sh [disk_image] [ovmf_code] [ovmf_vars] [test_type]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DISK_IMAGE="${1:-out/vm-disk.img}"
OVMF_CODE="${2:-}"
OVMF_VARS="${3:-}"
TEST_TYPE="${4:-default}"

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_step() {
    echo -e "${BLUE}▶${NC} $*"
}

log_success() {
    echo -e "${GREEN}✓${NC} $*"
}

log_warn() {
    echo -e "${YELLOW}⚠${NC} $*"
}

log_error() {
    echo -e "${RED}✗${NC} $*"
}

# Check prerequisites
log_step "Checking prerequisites..."

if [[ -z "$OVMF_CODE" ]] || [[ ! -f "$OVMF_CODE" ]]; then
    log_error "OVMF_CODE not found or not specified"
    exit 1
fi
log_success "OVMF code: $OVMF_CODE"

if [[ -z "$OVMF_VARS" ]] || [[ ! -f "$OVMF_VARS" ]]; then
    log_error "OVMF_VARS not found or not specified"
    exit 1
fi
log_success "OVMF vars: $OVMF_VARS"

if ! command -v qemu-system-x86_64 &> /dev/null; then
    log_error "qemu-system-x86_64 not found in PATH"
    exit 1
fi
log_success "QEMU available"

if [[ ! -f "$DISK_IMAGE" ]]; then
    log_error "Disk image not found: $DISK_IMAGE"
    log_step "To create disk, run: make vm-disk"
    exit 1
fi
log_success "Disk image: $DISK_IMAGE ($(du -h "$DISK_IMAGE" | cut -f1))"

echo ""
log_step "Starting VM test suite..."
echo ""

# Run the main test
if bash "$SCRIPT_DIR/run-vm-test.sh" "$DISK_IMAGE" "$OVMF_CODE" "$OVMF_VARS" 120; then
    log_success "VM test suite completed successfully"
    exit 0
else
    log_error "VM test suite failed"
    exit 1
fi
