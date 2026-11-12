CC ?= clang
AS = clang
KERNEL_LD ?= ld.lld

CFLAGS := -ffreestanding -fno-stack-protector -fno-pie -m64 -O2 -Wall -Wextra
ASFLAGS := -c
KERNEL_CFLAGS := --target=x86_64-unknown-elf -ffreestanding -fno-stack-protector -fpie -m64 -O2 -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-avx -Wall -Wextra
KERNEL_ASFLAGS := --target=x86_64-unknown-elf -c
FS_SELFTEST ?= 0
ifeq ($(FS_SELFTEST),1)
KERNEL_CFLAGS += -DKERNEL_FS_SELFTEST=1
endif
USER_SELFTEST ?= 0
ifneq ($(USER_SELFTEST),0)
KERNEL_CFLAGS += -DKERNEL_USER_SELFTEST=$(USER_SELFTEST)
endif
FAULT_TEST ?= 0
ifeq ($(FAULT_TEST),1)
KERNEL_CFLAGS += -DKERNEL_FAULT_TEST=1
endif
TESTS ?= 0
ifeq ($(TESTS),1)
KERNEL_CFLAGS += -DKERNEL_TESTS=1
endif
UEFI_CC ?= clang
UEFI_CFLAGS := --target=x86_64-unknown-windows-msvc -ffreestanding -fshort-wchar -fno-stack-protector -fno-pie -mno-red-zone -Wall -Wextra -O2
UEFI_LLD_LINK ?= lld-link
KERNEL_OBJCOPY ?= llvm-objcopy

OUT_DIR := out
BOOT_DIR := $(OUT_DIR)/bootloader
KERNEL_DIR := $(OUT_DIR)/kernel
APP_DIR := $(OUT_DIR)/apps/textedit
INSTALLER_DIR := $(OUT_DIR)/installer-layout
INSTALLER_PACKAGE_DIR := $(OUT_DIR)/installer-package
INSTALLER_PACKAGE_EFI_DIR := $(INSTALLER_PACKAGE_DIR)/EFI/BOOT
INSTALLER_PACKAGE_SCRIPT := $(INSTALLER_PACKAGE_DIR)/install.sh
UEFI_DIR := $(OUT_DIR)/uefi
UEFI_BOOT_DIR := $(UEFI_DIR)/EFI/BOOT

KERNEL_OBJ := $(KERNEL_DIR)/kernel.o
KERNEL_BIN := $(KERNEL_DIR)/kernel.bin
KERNEL_UEFI_STUB_OBJ := $(KERNEL_DIR)/uefi_kernel_stub.o
KERNEL_UEFI_MAIN_OBJ := $(KERNEL_DIR)/uefi_kernel_main.o
KERNEL_UEFI_CONSOLE_OBJ := $(KERNEL_DIR)/uefi_kernel_console.o
KERNEL_UEFI_FB_OBJ := $(KERNEL_DIR)/uefi_kernel_fb.o
KERNEL_UEFI_DESKTOP_OBJ := $(KERNEL_DIR)/uefi_kernel_desktop.o
KERNEL_UEFI_PS2_OBJ := $(KERNEL_DIR)/uefi_kernel_ps2.o
KERNEL_UEFI_WM_OBJ := $(KERNEL_DIR)/uefi_kernel_wm.o
KERNEL_UEFI_IRQ_OBJ := $(KERNEL_DIR)/uefi_kernel_irq.o
KERNEL_UEFI_IRQ_STUBS_OBJ := $(KERNEL_DIR)/uefi_kernel_irq_stubs.o
KERNEL_UEFI_RTC_OBJ := $(KERNEL_DIR)/uefi_kernel_rtc.o
KERNEL_UEFI_STRING_OBJ := $(KERNEL_DIR)/uefi_kernel_string.o
KERNEL_UEFI_PCI_OBJ := $(KERNEL_DIR)/uefi_kernel_pci.o
KERNEL_UEFI_AHCI_OBJ := $(KERNEL_DIR)/uefi_kernel_ahci.o
KERNEL_UEFI_BLOCK_OBJ := $(KERNEL_DIR)/uefi_kernel_block.o
KERNEL_UEFI_FAT32_OBJ := $(KERNEL_DIR)/uefi_kernel_fat32.o
KERNEL_UEFI_EXFAT_OBJ := $(KERNEL_DIR)/uefi_kernel_exfat.o
KERNEL_UEFI_FS_OBJ := $(KERNEL_DIR)/uefi_kernel_fs.o
KERNEL_UEFI_NVME_OBJ := $(KERNEL_DIR)/uefi_kernel_nvme.o
KERNEL_UEFI_USBSTOR_OBJ := $(KERNEL_DIR)/uefi_kernel_usbstor.o
KERNEL_UEFI_DISK_OBJ := $(KERNEL_DIR)/uefi_kernel_disk.o
KERNEL_UEFI_FORMAT_OBJ := $(KERNEL_DIR)/uefi_kernel_format.o
KERNEL_UEFI_SETUP_OBJ := $(KERNEL_DIR)/uefi_kernel_setup.o
KERNEL_UEFI_CONSOLEWIN_OBJ := $(KERNEL_DIR)/uefi_kernel_console_win.o
KERNEL_UEFI_SHELL_OBJ := $(KERNEL_DIR)/uefi_kernel_shell.o
KERNEL_UEFI_APPS_OBJ := $(KERNEL_DIR)/uefi_kernel_apps.o
KERNEL_UEFI_APPPROG_OBJ := $(KERNEL_DIR)/uefi_kernel_app_programs.o
KERNEL_UEFI_PMM_OBJ := $(KERNEL_DIR)/uefi_kernel_pmm.o
KERNEL_UEFI_HEAP_OBJ := $(KERNEL_DIR)/uefi_kernel_heap.o
KERNEL_UEFI_SCHEDULER_OBJ := $(KERNEL_DIR)/uefi_kernel_scheduler.o
KERNEL_UEFI_SCHEDULER_TEST_OBJ := $(KERNEL_DIR)/uefi_kernel_scheduler_test.o
KERNEL_UEFI_CONTEXT_SWITCH_OBJ := $(KERNEL_DIR)/uefi_kernel_context_switch.o
KERNEL_UEFI_CONTEXT_SWITCH_TEST_OBJ := $(KERNEL_DIR)/uefi_kernel_context_switch_test.o
KERNEL_UEFI_TCB_OBJ := $(KERNEL_DIR)/uefi_kernel_tcb.o
KERNEL_UEFI_TCB_TEST_OBJ := $(KERNEL_DIR)/uefi_kernel_tcb_test.o
KERNEL_UEFI_STACK_GUARD_OBJ := $(KERNEL_DIR)/uefi_kernel_stack_guard.o
KERNEL_UEFI_STACK_GUARD_TEST_OBJ := $(KERNEL_DIR)/uefi_kernel_stack_guard_test.o
KERNEL_UEFI_TIMER_OBJ := $(KERNEL_DIR)/uefi_kernel_timer.o
KERNEL_UEFI_TIMER_STUB_OBJ := $(KERNEL_DIR)/uefi_kernel_timer_stub.o
KERNEL_UEFI_TIMER_TEST_OBJ := $(KERNEL_DIR)/uefi_kernel_timer_test.o
KERNEL_UEFI_GDT_TEST_OBJ := $(KERNEL_DIR)/uefi_kernel_gdt_test.o
KERNEL_UEFI_PAGING_OBJ := $(KERNEL_DIR)/uefi_kernel_paging.o
KERNEL_UEFI_IDT_OBJ := $(KERNEL_DIR)/uefi_kernel_idt.o
KERNEL_UEFI_PIC_OBJ := $(KERNEL_DIR)/uefi_kernel_pic.o
KERNEL_UEFI_GDT_OBJ := $(KERNEL_DIR)/uefi_kernel_gdt.o
KERNEL_UEFI_USERMODE_OBJ := $(KERNEL_DIR)/uefi_kernel_usermode.o
KERNEL_UEFI_PROCESS_OBJ := $(KERNEL_DIR)/uefi_kernel_process.o
KERNEL_UEFI_SYSCALL_OBJ := $(KERNEL_DIR)/uefi_kernel_syscall.o
KERNEL_UEFI_SYSFILE_OBJ := $(KERNEL_DIR)/uefi_kernel_sysfile.o
KERNEL_UEFI_SYSWIN_OBJ := $(KERNEL_DIR)/uefi_kernel_syswin.o
KERNEL_UEFI_SYSCALL_ENTRY_OBJ := $(KERNEL_DIR)/uefi_kernel_syscall_entry.o
KERNEL_UEFI_EXCEPTIONS_OBJ := $(KERNEL_DIR)/uefi_kernel_exceptions.o
KERNEL_UEFI_ELF := $(KERNEL_DIR)/uefi_kernel.elf
KERNEL_UEFI_BIN := $(KERNEL_DIR)/uefi_kernel.bin
BOOT_OBJ := $(BOOT_DIR)/stage1.o
BOOT_BIN := $(BOOT_DIR)/stage1.bin
TEXTEDIT_OBJ := $(APP_DIR)/editor.o
UEFI_OBJ := $(UEFI_DIR)/bootx64.obj
UEFI_EFI := $(UEFI_BOOT_DIR)/BOOTX64.EFI
UEFI_KERNEL := $(UEFI_BOOT_DIR)/KERNEL.BIN

OVMF_CODE_CANDIDATES := OVMF/OVMF_CODE.fd /usr/share/OVMF/OVMF_CODE.fd /opt/homebrew/share/qemu/edk2-x86_64-code.fd
OVMF_VARS_TEMPLATE_CANDIDATES := OVMF/OVMF_VARS.fd /usr/share/OVMF/OVMF_VARS.fd /opt/homebrew/share/qemu/edk2-x86_64-vars.fd
OVMF_CODE ?= $(firstword $(wildcard $(OVMF_CODE_CANDIDATES)))
OVMF_VARS_TEMPLATE ?= $(firstword $(wildcard $(OVMF_VARS_TEMPLATE_CANDIDATES)))
OVMF_VARS := $(OUT_DIR)/OVMF_VARS.fd

VM_DISK := $(OUT_DIR)/vm-disk.img
VM_DISK_SIZE := 512M
VM_DISK_SIZE_MB := 512
VM_DISK_ESP_OFFSET_BYTES := 1048576
INSTALLER_SCRIPT := src/installer/install.sh

VBOX_VM_NAME ?= SecurePersonalOS
VBOX_DISK_RAW := $(OUT_DIR)/vbox-raw.img
# Store VDI outside out/ so 'make clean' doesn't break the VirtualBox VM reference
VBOX_DISK_VDI := vms/$(VBOX_VM_NAME).vdi
VBOX_DISK_SIZE_MB := 512
VBOX_MEM_MB := 512
# Byte offset of the ESP partition (LBA 2048 * 512 bytes/sector)
VBOX_ESP_OFFSET_BYTES := 1048576
QEMU_UI_FLAGS :=
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
QEMU_DISPLAY ?= cocoa
else
QEMU_DISPLAY ?= default
endif
QEMU_ZOOM_TO_FIT ?= on
QEMU_DISPLAY_EXTRA :=
QEMU_STORAGE_FLAGS := -drive if=none,format=raw,file=fat:rw:$(UEFI_DIR),id=esp -device virtio-blk-pci,drive=esp,bootindex=0
QEMU_BOOT_FLAGS := -boot order=c
QEMU_SERIAL_DEVICE ?= stdio
QEMU_SERIAL_FLAGS := -serial $(QEMU_SERIAL_DEVICE) -monitor none

ifeq ($(QEMU_DISPLAY),cocoa)
QEMU_DISPLAY_EXTRA := ,zoom-to-fit=$(QEMU_ZOOM_TO_FIT)
endif

QEMU_UI_FLAGS += -display $(QEMU_DISPLAY)$(QEMU_DISPLAY_EXTRA)

ifeq ($(QEMU_FULLSCREEN),1)
QEMU_UI_FLAGS += -full-screen
endif

QEMU_VM_DISK_FLAGS := -drive if=none,format=raw,file="$(VM_DISK)",id=vmdisk -device virtio-blk-pci,drive=vmdisk,bootindex=0

KERNEL_TEST_OBJS :=
ifeq ($(TESTS),1)
KERNEL_TEST_OBJS += $(KERNEL_UEFI_SCHEDULER_TEST_OBJ) $(KERNEL_UEFI_CONTEXT_SWITCH_TEST_OBJ) $(KERNEL_UEFI_TCB_TEST_OBJ) $(KERNEL_UEFI_STACK_GUARD_TEST_OBJ) $(KERNEL_UEFI_TIMER_TEST_OBJ) $(KERNEL_UEFI_GDT_TEST_OBJ)
endif

.PHONY: help all check-tools check-uefi-tools check-qemu-tools check-vm-disk-tools check-vbox-tools dirs kernel bootloader textedit installer-layout installer-package uefi-loader qemu-run test-run usb-image vm-disk vm-test vm-shell fault-test vbox-disk vbox-vm-create vbox-vm-delete vbox-run clean

help:
	@echo "Targets:"
	@echo "  qemu-run          Production boot (tests disabled by default)"
	@echo "                    (set QEMU_FULLSCREEN=1 for fullscreen window)"
	@echo "                    (set QEMU_DISPLAY=cocoa QEMU_ZOOM_TO_FIT=on|off on macOS)"
	@echo "  test-run          Test boot (same as TESTS=1 qemu-run)"
	@echo "  uefi-loader       Build EFI/BOOT/BOOTX64.EFI and stage KERNEL.BIN"
	@echo "                    (set TESTS=1 to include kernel test suites)"
	@echo "  check-tools       Verify required host tools exist"
	@echo "  kernel            Build freestanding kernel object/BIN"
	@echo "  bootloader        Build stage1 boot assembly binary"
	@echo "  textedit          Build text editor assembly object"
	@echo "  installer-layout  Stage artifacts into installer layout folder"
	@echo "  installer-package Stage UEFI payload + install.sh for deployment"
	@echo "  usb-image         Build out/winclone-usb.img (GPT + FAT32 ESP) for a USB stick; Python only"
	@echo "  vm-disk           Create bootable QEMU disk image with installer"
	@echo "  vm-test           Automated VM test suite (boot → kernel validation)"
	@echo "  vm-shell          Interactive QEMU session for manual testing"
	@echo "  fault-test        Boot with an intentional fault test enabled"
	@echo "  vbox-disk         Create bootable VirtualBox VDI (requires gptfdisk + mtools)"
	@echo "                    brew install gptfdisk mtools"
	@echo "  vbox-vm-create    Register and configure VirtualBox VM (run once)"
	@echo "  vbox-vm-delete    Unregister and delete the VirtualBox VM"
	@echo "                    (set VBOX_VM_NAME=... to use a custom VM name)"
	@echo "  vbox-run          Start the VirtualBox VM"
	@echo "  all               Run week-1 build flow"
	@echo "  clean             Delete all generated artifacts"

all: check-tools kernel bootloader textedit installer-layout

check-tools:
	@command -v $(CC) >/dev/null || (echo "Missing compiler: $(CC)" && exit 1)
	@command -v $(AS) >/dev/null || (echo "Missing assembler frontend: $(AS)" && exit 1)

check-uefi-tools:
	@command -v $(UEFI_CC) >/dev/null || (echo "Missing UEFI C compiler: $(UEFI_CC)" && exit 1)
	@command -v $(UEFI_LLD_LINK) >/dev/null || (echo "Missing PE/COFF linker: $(UEFI_LLD_LINK)" && exit 1)
	@command -v $(KERNEL_OBJCOPY) >/dev/null || (echo "Missing objcopy tool: $(KERNEL_OBJCOPY)" && exit 1)

check-qemu-tools:
	@command -v qemu-system-x86_64 >/dev/null || (echo "Missing emulator: qemu-system-x86_64" && exit 1)
	@test -n "$(OVMF_CODE)" || (echo "Missing OVMF code image. Place one at OVMF/OVMF_CODE.fd or pass OVMF_CODE=/path/to/OVMF_CODE.fd" && exit 1)
	@test -n "$(OVMF_VARS_TEMPLATE)" || (echo "Missing OVMF vars template. Place one at OVMF/OVMF_VARS.fd or pass OVMF_VARS_TEMPLATE=/path/to/OVMF_VARS.fd" && exit 1)
	@test -f "$(OVMF_CODE)" || (echo "Missing OVMF code image: $(OVMF_CODE)" && exit 1)
	@test -f "$(OVMF_VARS_TEMPLATE)" || (echo "Missing OVMF vars template: $(OVMF_VARS_TEMPLATE)" && exit 1)

check-vm-disk-tools:
	@command -v sgdisk >/dev/null || (echo "Missing sgdisk: brew install gptfdisk" && exit 1)
	@command -v mformat >/dev/null || (echo "Missing mtools: brew install mtools" && exit 1)
	@command -v mmd >/dev/null || (echo "Missing mtools: brew install mtools" && exit 1)
	@command -v mcopy >/dev/null || (echo "Missing mtools: brew install mtools" && exit 1)

dirs:
	@mkdir -p $(BOOT_DIR) $(KERNEL_DIR) $(APP_DIR) $(INSTALLER_DIR)/boot $(INSTALLER_DIR)/kernel $(INSTALLER_DIR)/system/apps/textedit $(INSTALLER_DIR)/system/installer $(INSTALLER_PACKAGE_EFI_DIR) $(UEFI_BOOT_DIR) vms

$(KERNEL_OBJ): src/kernel/main.c
	@mkdir -p $(KERNEL_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL_BIN): $(KERNEL_OBJ)
	cp $< $@

kernel: $(KERNEL_OBJ) $(KERNEL_BIN)

$(KERNEL_UEFI_STUB_OBJ): src/kernel/uefi_kernel.S | dirs
	$(CC) $(KERNEL_ASFLAGS) $< -o $@

$(KERNEL_UEFI_MAIN_OBJ): src/kernel/main.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_CONSOLE_OBJ): src/kernel/console.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_FB_OBJ): src/kernel/fb.c src/kernel/fb.h src/kernel/font8x8.h $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_DESKTOP_OBJ): src/kernel/desktop.c src/kernel/desktop.h src/kernel/fb.h src/kernel/ps2.h src/kernel/wm.h src/kernel/apps.h $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_STRING_OBJ): src/kernel/string.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_PCI_OBJ): src/kernel/pci.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_AHCI_OBJ): src/kernel/ahci.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_BLOCK_OBJ): src/kernel/block.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_FAT32_OBJ): src/kernel/fat32.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_EXFAT_OBJ): src/kernel/exfat.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_FS_OBJ): src/kernel/fs.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_USBSTOR_OBJ): src/kernel/usbstor.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_NVME_OBJ): src/kernel/nvme.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_DISK_OBJ): src/kernel/disk.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_FORMAT_OBJ): src/kernel/format.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_SETUP_OBJ): src/kernel/setup.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_CONSOLEWIN_OBJ): src/kernel/console_win.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_SHELL_OBJ): src/kernel/shell.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_IRQ_OBJ): src/kernel/irq.c src/kernel/irq.h src/kernel/pic.h $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_IRQ_STUBS_OBJ): src/kernel/irq_stubs.S | dirs
	$(CC) $(KERNEL_ASFLAGS) $< -o $@

$(KERNEL_UEFI_RTC_OBJ): src/kernel/rtc.c src/kernel/rtc.h $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_WM_OBJ): src/kernel/wm.c src/kernel/wm.h src/kernel/fb.h $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_APPPROG_OBJ): src/kernel/app_programs.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_APPS_OBJ): src/kernel/apps.c src/kernel/apps.h src/kernel/wm.h src/kernel/fb.h $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_PS2_OBJ): src/kernel/ps2.c src/kernel/ps2.h $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_PMM_OBJ): src/kernel/pmm.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_HEAP_OBJ): src/kernel/heap.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_PAGING_OBJ): src/kernel/paging.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_IDT_OBJ): src/kernel/idt.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_PIC_OBJ): src/kernel/pic.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_GDT_OBJ): src/kernel/gdt.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_USERMODE_OBJ): src/kernel/usermode.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_PROCESS_OBJ): src/kernel/process.c src/kernel/process.h $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_SYSCALL_OBJ): src/kernel/syscall.c src/kernel/sysfile.h $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_SYSFILE_OBJ): src/kernel/sysfile.c src/kernel/sysfile.h src/kernel/syswin.h $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_SYSWIN_OBJ): src/kernel/syswin.c src/kernel/syswin.h src/kernel/font8x8.h src/kernel/fb.h src/kernel/wm.h $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_SYSCALL_ENTRY_OBJ): src/kernel/syscall_entry.S | dirs
	$(CC) $(KERNEL_ASFLAGS) $< -o $@

$(KERNEL_UEFI_EXCEPTIONS_OBJ): src/kernel/exceptions.S | dirs
	$(CC) $(KERNEL_ASFLAGS) $< -o $@

$(KERNEL_UEFI_SCHEDULER_OBJ): src/kernel/scheduler.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_SCHEDULER_TEST_OBJ): src/kernel/test_scheduler.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_CONTEXT_SWITCH_OBJ): src/kernel/context_switch.S | dirs
	$(CC) $(KERNEL_ASFLAGS) $< -o $@

$(KERNEL_UEFI_CONTEXT_SWITCH_TEST_OBJ): src/kernel/test_context_switch.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_TCB_OBJ): src/kernel/tcb.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_TCB_TEST_OBJ): src/kernel/test_tcb.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_STACK_GUARD_OBJ): src/kernel/stack_guard.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_STACK_GUARD_TEST_OBJ): src/kernel/test_stack_guard.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_TIMER_OBJ): src/kernel/timer.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_TIMER_STUB_OBJ): src/kernel/timer_stub.S | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_TIMER_TEST_OBJ): src/kernel/test_timer.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_GDT_TEST_OBJ): src/kernel/test_gdt.c $(wildcard src/kernel/*.h) | dirs
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_UEFI_ELF): $(KERNEL_UEFI_STUB_OBJ) $(KERNEL_UEFI_MAIN_OBJ) $(KERNEL_UEFI_CONSOLE_OBJ) $(KERNEL_UEFI_FB_OBJ) $(KERNEL_UEFI_DESKTOP_OBJ) $(KERNEL_UEFI_PS2_OBJ) $(KERNEL_UEFI_WM_OBJ) $(KERNEL_UEFI_APPS_OBJ) $(KERNEL_UEFI_APPPROG_OBJ) $(KERNEL_UEFI_IRQ_OBJ) $(KERNEL_UEFI_IRQ_STUBS_OBJ) $(KERNEL_UEFI_RTC_OBJ) $(KERNEL_UEFI_STRING_OBJ) $(KERNEL_UEFI_PCI_OBJ) $(KERNEL_UEFI_AHCI_OBJ) $(KERNEL_UEFI_BLOCK_OBJ) $(KERNEL_UEFI_FAT32_OBJ) $(KERNEL_UEFI_EXFAT_OBJ) $(KERNEL_UEFI_FS_OBJ) $(KERNEL_UEFI_NVME_OBJ) $(KERNEL_UEFI_USBSTOR_OBJ) $(KERNEL_UEFI_DISK_OBJ) $(KERNEL_UEFI_FORMAT_OBJ) $(KERNEL_UEFI_SETUP_OBJ) $(KERNEL_UEFI_CONSOLEWIN_OBJ) $(KERNEL_UEFI_SHELL_OBJ) $(KERNEL_UEFI_PMM_OBJ) $(KERNEL_UEFI_HEAP_OBJ) $(KERNEL_UEFI_PAGING_OBJ) $(KERNEL_UEFI_IDT_OBJ) $(KERNEL_UEFI_PIC_OBJ) $(KERNEL_UEFI_GDT_OBJ) $(KERNEL_UEFI_USERMODE_OBJ) $(KERNEL_UEFI_PROCESS_OBJ) $(KERNEL_UEFI_SYSCALL_OBJ) $(KERNEL_UEFI_SYSFILE_OBJ) $(KERNEL_UEFI_SYSWIN_OBJ) $(KERNEL_UEFI_SYSCALL_ENTRY_OBJ) $(KERNEL_UEFI_EXCEPTIONS_OBJ) $(KERNEL_UEFI_SCHEDULER_OBJ) $(KERNEL_UEFI_CONTEXT_SWITCH_OBJ) $(KERNEL_UEFI_TCB_OBJ) $(KERNEL_UEFI_STACK_GUARD_OBJ) $(KERNEL_UEFI_TIMER_OBJ) $(KERNEL_UEFI_TIMER_STUB_OBJ) $(KERNEL_TEST_OBJS)
	$(KERNEL_LD) -pie -T src/kernel/kernel.ld -o $@ $^

$(KERNEL_UEFI_BIN): $(KERNEL_UEFI_ELF)
	$(KERNEL_OBJCOPY) -O binary $< $@
	python3 -c "import os,struct; f=open('$@','r+b'); f.seek(24); f.write(struct.pack('<Q',os.path.getsize('$@'))); f.close()"

$(BOOT_OBJ): src/boot/stage1.S | dirs
	$(AS) $(ASFLAGS) $< -o $@

$(BOOT_BIN): $(BOOT_OBJ)
	cp $< $@

bootloader: $(BOOT_OBJ) $(BOOT_BIN)

$(TEXTEDIT_OBJ): src/apps/textedit/editor.S | dirs
	$(AS) $(ASFLAGS) $< -o $@

textedit: $(TEXTEDIT_OBJ)

installer-layout: kernel bootloader textedit | dirs
	cp $(BOOT_BIN) $(INSTALLER_DIR)/boot/
	cp $(KERNEL_BIN) $(INSTALLER_DIR)/kernel/
	cp $(TEXTEDIT_OBJ) $(INSTALLER_DIR)/system/apps/textedit/
	cp $(INSTALLER_SCRIPT) $(INSTALLER_DIR)/system/installer/install.sh
	chmod +x $(INSTALLER_DIR)/system/installer/install.sh
	@echo "Installer staging created at: $(INSTALLER_DIR)"

installer-package: uefi-loader $(INSTALLER_SCRIPT) | dirs
	cp $(UEFI_EFI) $(INSTALLER_PACKAGE_EFI_DIR)/BOOTX64.EFI
	cp $(UEFI_KERNEL) $(INSTALLER_PACKAGE_EFI_DIR)/KERNEL.BIN
	@mkdir -p $(INSTALLER_PACKAGE_DIR)/APPS
	cp $(UEFI_DIR)/APPS/*.BIN $(INSTALLER_PACKAGE_DIR)/APPS/
	-cp $(UEFI_DIR)/APPS/*.EXE $(UEFI_DIR)/APPS/*.DLL $(INSTALLER_PACKAGE_DIR)/APPS/
	cp $(INSTALLER_SCRIPT) $(INSTALLER_PACKAGE_SCRIPT)
	chmod +x $(INSTALLER_PACKAGE_SCRIPT)
	@echo "Installer package created at: $(INSTALLER_PACKAGE_DIR)"

$(UEFI_OBJ): src/boot/uefi_main.c | dirs
	$(UEFI_CC) $(UEFI_CFLAGS) -c $< -o $@

$(UEFI_EFI): $(UEFI_OBJ)
	$(UEFI_LLD_LINK) /subsystem:efi_application /entry:efi_main /nodefaultlib /out:$@ $<

$(UEFI_KERNEL): $(KERNEL_UEFI_BIN) | dirs
	cp $< $@

uefi-loader: check-uefi-tools $(UEFI_EFI) $(UEFI_KERNEL) user-apps win32-apps
	@echo "UEFI loader built at: $(UEFI_EFI)"
	@echo "UEFI kernel staged at: $(UEFI_KERNEL)"

$(OVMF_VARS): | dirs check-qemu-tools
	cp "$(OVMF_VARS_TEMPLATE)" "$(OVMF_VARS)"

# ---- User programs (ELF executables loaded by the kernel; see src/user/) --------
USER_DIR := $(OUT_DIR)/user
USER_CFLAGS := --target=x86_64-unknown-elf -ffreestanding -fno-stack-protector -fpie -m64 -O2 -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-avx -Wall -Wextra -Isrc/user

$(USER_DIR)/%.o: src/user/%.c src/user/user.h
	@mkdir -p $(USER_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_DIR)/%.elf: $(USER_DIR)/%.o $(USER_DIR)/crt0.o src/user/user.ld
	$(KERNEL_LD) -z max-page-size=4096 -T src/user/user.ld -o $@ $< $(USER_DIR)/crt0.o

$(USER_DIR)/%.bin: $(USER_DIR)/%.elf
	$(KERNEL_OBJCOPY) --strip-all $< $@

# Each program is staged under an upper-case 8.3 name on the boot volume.
user-apps: $(USER_DIR)/hello.bin $(USER_DIR)/count.bin $(USER_DIR)/crash.bin $(USER_DIR)/echo.bin $(USER_DIR)/tick.bin $(USER_DIR)/iso.bin $(USER_DIR)/files.bin $(USER_DIR)/wintest.bin $(USER_DIR)/winev.bin $(USER_DIR)/windemo.bin $(USER_DIR)/notes.bin $(USER_DIR)/explorer.bin $(USER_DIR)/paint.bin $(USER_DIR)/about.bin $(USER_DIR)/reszt.bin $(USER_DIR)/probe.bin $(USER_DIR)/stackovf.bin $(USER_DIR)/spin.bin $(USER_DIR)/kfault.bin $(USER_DIR)/toast.bin
	@mkdir -p $(UEFI_DIR)/APPS
	cp $(USER_DIR)/hello.bin $(UEFI_DIR)/APPS/HELLO.BIN
	cp $(USER_DIR)/count.bin $(UEFI_DIR)/APPS/COUNT.BIN
	cp $(USER_DIR)/crash.bin $(UEFI_DIR)/APPS/CRASH.BIN
	cp $(USER_DIR)/echo.bin $(UEFI_DIR)/APPS/ECHO.BIN
	cp $(USER_DIR)/tick.bin $(UEFI_DIR)/APPS/TICK.BIN
	cp $(USER_DIR)/iso.bin $(UEFI_DIR)/APPS/ISO.BIN
	cp $(USER_DIR)/files.bin $(UEFI_DIR)/APPS/FILES.BIN
	cp $(USER_DIR)/wintest.bin $(UEFI_DIR)/APPS/WINTEST.BIN
	cp $(USER_DIR)/winev.bin $(UEFI_DIR)/APPS/WINEV.BIN
	cp $(USER_DIR)/windemo.bin $(UEFI_DIR)/APPS/WINDEMO.BIN
	cp $(USER_DIR)/notes.bin $(UEFI_DIR)/APPS/NOTES.BIN
	cp $(USER_DIR)/explorer.bin $(UEFI_DIR)/APPS/EXPLORER.BIN
	cp $(USER_DIR)/paint.bin $(UEFI_DIR)/APPS/PAINT.BIN
	cp $(USER_DIR)/about.bin $(UEFI_DIR)/APPS/ABOUT.BIN
	cp $(USER_DIR)/reszt.bin $(UEFI_DIR)/APPS/RESZT.BIN
	cp $(USER_DIR)/probe.bin $(UEFI_DIR)/APPS/PROBE.BIN
	cp $(USER_DIR)/stackovf.bin $(UEFI_DIR)/APPS/STACKOVF.BIN
	cp $(USER_DIR)/spin.bin $(UEFI_DIR)/APPS/SPIN.BIN
	cp $(USER_DIR)/toast.bin $(UEFI_DIR)/APPS/TOAST.BIN
	cp $(USER_DIR)/kfault.bin $(UEFI_DIR)/APPS/KFAULT.BIN

.PHONY: user-apps

# ---- Windows (PE) programs and the Win32 runtime (src/win32) ---------------------------------
# WIN32.DLL (kernel32 + user32 + gdi32) and the test programs are built with clang for the
# Windows target against the real Windows SDK headers and import libraries (Visual Studio's
# C++ headers plus a Windows 10/11 SDK). Without them the step is skipped and the OS builds as
# before; programs that need the runtime then do not start. Override WINSDK_ROOT, WINSDK_VER
# and MSVC_ROOT if yours are elsewhere.
WIN32_DIR := $(OUT_DIR)/win32
WINSDK_ROOT ?= C:/Program Files (x86)/Windows Kits/10
WINSDK_VER ?= 10.0.26100.0
MSVC_ROOT ?= $(shell ls -d "C:/Program Files/Microsoft Visual Studio/2022/"*/VC/Tools/MSVC/* 2>/dev/null | sort | tail -n 1)
WIN32_SDK_OK := $(shell test -f "$(WINSDK_ROOT)/Include/$(WINSDK_VER)/um/windows.h" && test -f "$(MSVC_ROOT)/include/excpt.h" && echo 1)
WIN32_CFLAGS := --target=x86_64-pc-windows-msvc -ffreestanding -fno-builtin -fno-stack-protector -funwind-tables -mno-sse -O2 -Wall -Wextra -Wno-unused-parameter -Wno-microsoft
WIN32_INCLUDES := -isystem "$(MSVC_ROOT)/include" -isystem "$(WINSDK_ROOT)/Include/$(WINSDK_VER)/um" -isystem "$(WINSDK_ROOT)/Include/$(WINSDK_VER)/shared" -isystem "$(WINSDK_ROOT)/Include/$(WINSDK_VER)/ucrt"
WIN32_LIBPATH := "-libpath:$(WINSDK_ROOT)/Lib/$(WINSDK_VER)/um/x64"
WIN32_CRT_LIBPATH := "-libpath:$(MSVC_ROOT)/lib/x64" "-libpath:$(WINSDK_ROOT)/Lib/$(WINSDK_VER)/ucrt/x64"
# The C runtime part of the runtime uses SSE (doubles travel in XMM registers) and declares the
# UCRT functions itself: the UCRT headers would mark them dllimport.
WIN32_SSE_CFLAGS := $(filter-out -mno-sse,$(WIN32_CFLAGS)) -fno-math-errno -D_CRT_SECURE_NO_WARNINGS
WIN32_RT_OBJS := $(WIN32_DIR)/kernel32.obj $(WIN32_DIR)/user32.obj $(WIN32_DIR)/gdi32.obj $(WIN32_DIR)/bitmap.obj $(WIN32_DIR)/controls.obj $(WIN32_DIR)/menu.obj $(WIN32_DIR)/dialog.obj $(WIN32_DIR)/crt.obj $(WIN32_DIR)/crtfmt.obj $(WIN32_DIR)/crtmath.obj $(WIN32_DIR)/threads.obj $(WIN32_DIR)/seh.obj $(WIN32_DIR)/cxxeh.obj
WIN32_TESTS := hello badimp wingui winctl winmenu windlg wincombo winscroll winsbar winbmp winmsg fptest crttest thrtest cxxtest
WIN32_EXES := $(patsubst %,$(WIN32_DIR)/%.exe,$(WIN32_TESTS))

$(WIN32_DIR)/%.obj: src/win32/%.c src/win32/internal.h
	@mkdir -p $(WIN32_DIR)
	$(CC) $(WIN32_CFLAGS) $(WIN32_INCLUDES) -c $< -o $@

$(WIN32_DIR)/t_%.obj: src/win32/tests/%.c
	@mkdir -p $(WIN32_DIR)
	$(CC) $(WIN32_CFLAGS) $(WIN32_INCLUDES) -c $< -o $@

$(WIN32_DIR)/crt.obj $(WIN32_DIR)/crtfmt.obj $(WIN32_DIR)/crtmath.obj: $(WIN32_DIR)/%.obj: src/win32/%.c src/win32/internal.h src/win32/crt.h
	@mkdir -p $(WIN32_DIR)
	$(CC) $(WIN32_SSE_CFLAGS) $(WIN32_INCLUDES) -c $< -o $@

$(WIN32_DIR)/win32.dll: $(WIN32_RT_OBJS)
	$(UEFI_LLD_LINK) -dll -noentry -nodefaultlib -opt:ref -export:memcpy -export:memmove -export:memset -export:memcmp -out:$@ $^

$(WIN32_DIR)/missing.lib: src/win32/tests/missing.def
	@mkdir -p $(WIN32_DIR)
	llvm-lib -def:$< -machine:x64 -out:$@

$(WIN32_DIR)/badimp.exe: $(WIN32_DIR)/t_badimp.obj $(WIN32_DIR)/t_crt.obj $(WIN32_DIR)/t_entry_console.obj $(WIN32_DIR)/missing.lib
	$(UEFI_LLD_LINK) -subsystem:console -entry:mainCRTStartup -nodefaultlib -out:$@ $(filter %.obj,$^) $(filter %.lib,$^) $(WIN32_LIBPATH) kernel32.lib

$(WIN32_DIR)/hello.exe: $(WIN32_DIR)/t_hello.obj $(WIN32_DIR)/t_crt.obj $(WIN32_DIR)/t_entry_console.obj
	$(UEFI_LLD_LINK) -subsystem:console -entry:mainCRTStartup -nodefaultlib -out:$@ $^ $(WIN32_LIBPATH) kernel32.lib user32.lib

$(WIN32_DIR)/wingui.exe: $(WIN32_DIR)/t_wingui.obj $(WIN32_DIR)/t_crt.obj $(WIN32_DIR)/t_entry_gui.obj
	$(UEFI_LLD_LINK) -subsystem:windows -entry:WinMainCRTStartup -nodefaultlib -out:$@ $^ $(WIN32_LIBPATH) kernel32.lib user32.lib gdi32.lib

# Built with SSE on (WIN32_CFLAGS has it off): exercises the per-task x87/SSE state.
$(WIN32_DIR)/t_fptest.obj: src/win32/tests/fptest.c
	@mkdir -p $(WIN32_DIR)
	$(CC) $(filter-out -mno-sse,$(WIN32_CFLAGS)) $(WIN32_INCLUDES) -c $< -o $@

$(WIN32_DIR)/fptest.exe: $(WIN32_DIR)/t_fptest.obj $(WIN32_DIR)/t_crt.obj $(WIN32_DIR)/t_entry_console.obj
	$(UEFI_LLD_LINK) -subsystem:console -entry:mainCRTStartup -nodefaultlib -out:$@ $^ $(WIN32_LIBPATH) kernel32.lib user32.lib

# A program built the way Visual Studio builds it: the dynamic C runtime (/MD), its own startup
# code and the real UCRT headers. No CRT replacement files: msvcrt.lib/vcruntime.lib/ucrt.lib.
$(WIN32_DIR)/t_crttest.obj: src/win32/tests/crttest.c
	@mkdir -p $(WIN32_DIR)
	$(CC) --target=x86_64-pc-windows-msvc -O1 -fno-stack-protector -Wall -Wno-microsoft -Wno-deprecated-declarations $(WIN32_INCLUDES) -c $< -o $@

$(WIN32_DIR)/crttest.exe: $(WIN32_DIR)/t_crttest.obj
	$(UEFI_LLD_LINK) -subsystem:console -entry:mainCRTStartup -nodefaultlib -out:$@ $^ $(WIN32_LIBPATH) $(WIN32_CRT_LIBPATH) msvcrt.lib vcruntime.lib ucrt.lib kernel32.lib

$(WIN32_DIR)/t_thrtest.obj: src/win32/tests/thrtest.c
	@mkdir -p $(WIN32_DIR)
	$(CC) --target=x86_64-pc-windows-msvc -O1 -fno-stack-protector -Wall -Wno-microsoft -Wno-deprecated-declarations $(WIN32_INCLUDES) -c $< -o $@

$(WIN32_DIR)/thrtest.exe: $(WIN32_DIR)/t_thrtest.obj
	$(UEFI_LLD_LINK) -subsystem:console -entry:mainCRTStartup -nodefaultlib -out:$@ $^ $(WIN32_LIBPATH) $(WIN32_CRT_LIBPATH) msvcrt.lib vcruntime.lib ucrt.lib kernel32.lib

# C++ with exceptions (clang emits the FH3 tables the runtime reads) and __try/__except.
$(WIN32_DIR)/t_cxxtest.obj: src/win32/tests/cxxtest.cpp
	@mkdir -p $(WIN32_DIR)
	$(CC) --target=x86_64-pc-windows-msvc -x c++ -std=c++17 -O1 -fexceptions -fcxx-exceptions -fms-extensions -fno-stack-protector -Wall -Wno-microsoft -Wno-deprecated-declarations $(WIN32_INCLUDES) -c $< -o $@

$(WIN32_DIR)/cxxtest.exe: $(WIN32_DIR)/t_cxxtest.obj
	$(UEFI_LLD_LINK) -subsystem:console -entry:mainCRTStartup -nodefaultlib -out:$@ $^ $(WIN32_LIBPATH) $(WIN32_CRT_LIBPATH) msvcrt.lib vcruntime.lib ucrt.lib kernel32.lib

$(WIN32_DIR)/winctl.exe: $(WIN32_DIR)/t_winctl.obj $(WIN32_DIR)/t_crt.obj $(WIN32_DIR)/t_entry_gui.obj
	$(UEFI_LLD_LINK) -subsystem:windows -entry:WinMainCRTStartup -nodefaultlib -out:$@ $^ $(WIN32_LIBPATH) kernel32.lib user32.lib gdi32.lib

$(WIN32_DIR)/winmenu.exe: $(WIN32_DIR)/t_winmenu.obj $(WIN32_DIR)/t_crt.obj $(WIN32_DIR)/t_entry_gui.obj
	$(UEFI_LLD_LINK) -subsystem:windows -entry:WinMainCRTStartup -nodefaultlib -out:$@ $^ $(WIN32_LIBPATH) kernel32.lib user32.lib gdi32.lib

# Resources are compiled by llvm-rc and linked as a .res file.
$(WIN32_DIR)/windlg.res: src/win32/tests/windlg.rc
	@mkdir -p $(WIN32_DIR)
	llvm-rc -FO $@ $<

$(WIN32_DIR)/windlg.exe: $(WIN32_DIR)/t_windlg.obj $(WIN32_DIR)/windlg.res $(WIN32_DIR)/t_crt.obj $(WIN32_DIR)/t_entry_gui.obj
	$(UEFI_LLD_LINK) -subsystem:windows -entry:WinMainCRTStartup -nodefaultlib -out:$@ $^ $(WIN32_LIBPATH) kernel32.lib user32.lib gdi32.lib

$(WIN32_DIR)/winscroll.exe: $(WIN32_DIR)/t_winscroll.obj $(WIN32_DIR)/t_crt.obj $(WIN32_DIR)/t_entry_gui.obj
	$(UEFI_LLD_LINK) -subsystem:windows -entry:WinMainCRTStartup -nodefaultlib -out:$@ $^ $(WIN32_LIBPATH) kernel32.lib user32.lib gdi32.lib

$(WIN32_DIR)/winsbar.exe: $(WIN32_DIR)/t_winsbar.obj $(WIN32_DIR)/t_crt.obj $(WIN32_DIR)/t_entry_gui.obj
	$(UEFI_LLD_LINK) -subsystem:windows -entry:WinMainCRTStartup -nodefaultlib -out:$@ $^ $(WIN32_LIBPATH) kernel32.lib user32.lib gdi32.lib

$(WIN32_DIR)/winbmp.res: src/win32/tests/winbmp.rc src/win32/tests/pic.bmp
	@mkdir -p $(WIN32_DIR)
	llvm-rc -FO $@ $<

$(WIN32_DIR)/winbmp.exe: $(WIN32_DIR)/t_winbmp.obj $(WIN32_DIR)/winbmp.res $(WIN32_DIR)/t_crt.obj $(WIN32_DIR)/t_entry_gui.obj
	$(UEFI_LLD_LINK) -subsystem:windows -entry:WinMainCRTStartup -nodefaultlib -out:$@ $^ $(WIN32_LIBPATH) kernel32.lib user32.lib gdi32.lib

$(WIN32_DIR)/wincombo.exe: $(WIN32_DIR)/t_wincombo.obj $(WIN32_DIR)/t_crt.obj $(WIN32_DIR)/t_entry_gui.obj
	$(UEFI_LLD_LINK) -subsystem:windows -entry:WinMainCRTStartup -nodefaultlib -out:$@ $^ $(WIN32_LIBPATH) kernel32.lib user32.lib gdi32.lib

$(WIN32_DIR)/winmsg.exe: $(WIN32_DIR)/t_winmsg.obj $(WIN32_DIR)/t_crt.obj $(WIN32_DIR)/t_entry_gui.obj
	$(UEFI_LLD_LINK) -subsystem:windows -entry:WinMainCRTStartup -nodefaultlib -out:$@ $^ $(WIN32_LIBPATH) kernel32.lib user32.lib

ifeq ($(WIN32_SDK_OK),1)
win32-apps: $(WIN32_DIR)/win32.dll $(WIN32_EXES)
	@mkdir -p $(UEFI_DIR)/APPS
	cp $(WIN32_DIR)/win32.dll $(UEFI_DIR)/APPS/WIN32.DLL
	cp $(WIN32_DIR)/hello.exe $(UEFI_DIR)/APPS/WINHELLO.EXE
	cp $(WIN32_DIR)/badimp.exe $(UEFI_DIR)/APPS/WINBAD.EXE
	cp $(WIN32_DIR)/wingui.exe $(UEFI_DIR)/APPS/WINGUI.EXE
	cp $(WIN32_DIR)/winctl.exe $(UEFI_DIR)/APPS/WINCTL.EXE
	cp $(WIN32_DIR)/winmenu.exe $(UEFI_DIR)/APPS/WINMENU.EXE
	cp $(WIN32_DIR)/windlg.exe $(UEFI_DIR)/APPS/WINDLG.EXE
	cp $(WIN32_DIR)/wincombo.exe $(UEFI_DIR)/APPS/WINCOMBO.EXE
	cp $(WIN32_DIR)/winscroll.exe $(UEFI_DIR)/APPS/WINSCROLL.EXE
	cp $(WIN32_DIR)/winsbar.exe $(UEFI_DIR)/APPS/WINSBAR.EXE
	cp $(WIN32_DIR)/winbmp.exe $(UEFI_DIR)/APPS/WINBMP.EXE
	cp $(WIN32_DIR)/winmsg.exe $(UEFI_DIR)/APPS/WINMSG.EXE
	cp $(WIN32_DIR)/fptest.exe $(UEFI_DIR)/APPS/WINFP.EXE
	cp $(WIN32_DIR)/crttest.exe $(UEFI_DIR)/APPS/WINCRT.EXE
	cp $(WIN32_DIR)/thrtest.exe $(UEFI_DIR)/APPS/WINTHR.EXE
	cp $(WIN32_DIR)/cxxtest.exe $(UEFI_DIR)/APPS/WINCXX.EXE
else
win32-apps:
	@echo "win32-apps: Windows SDK or Visual Studio headers not found; skipping the Win32 runtime and PE test programs (see WINSDK_ROOT in the Makefile)"
endif

.PHONY: win32-apps

qemu-run: uefi-loader $(OVMF_VARS) check-qemu-tools
	qemu-system-x86_64 \
		-machine q35 \
		-m 512M \
		$(QEMU_BOOT_FLAGS) \
		$(QEMU_UI_FLAGS) \
		$(QEMU_SERIAL_FLAGS) \
		-drive if=pflash,format=raw,readonly=on,file="$(OVMF_CODE)" \
		-drive if=pflash,format=raw,file="$(OVMF_VARS)" \
		$(QEMU_STORAGE_FLAGS)

test-run:
	@$(MAKE) TESTS=1 qemu-run

usb-image: uefi-loader
	python scripts/make-usb-image.py $(OUT_DIR)/winclone-usb.img

$(VM_DISK): installer-package | dirs check-vm-disk-tools
	@echo "Creating bootable VM disk image..."
	@rm -f $(VM_DISK)
	@dd if=/dev/zero of=$(VM_DISK) bs=1M count=$(VM_DISK_SIZE_MB) 2>/dev/null
	sgdisk -n 1:2048:+200M -t 1:EF00 -c 1:EFI $(VM_DISK)
	mformat -i $(VM_DISK)@@$(VM_DISK_ESP_OFFSET_BYTES) -F ::
	mmd -i $(VM_DISK)@@$(VM_DISK_ESP_OFFSET_BYTES) ::/EFI ::/EFI/BOOT
	mcopy -i $(VM_DISK)@@$(VM_DISK_ESP_OFFSET_BYTES) \
		$(INSTALLER_PACKAGE_EFI_DIR)/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i $(VM_DISK)@@$(VM_DISK_ESP_OFFSET_BYTES) \
		$(INSTALLER_PACKAGE_EFI_DIR)/KERNEL.BIN ::/EFI/BOOT/KERNEL.BIN
	mmd -i $(VM_DISK)@@$(VM_DISK_ESP_OFFSET_BYTES) ::/APPS
	mcopy -i $(VM_DISK)@@$(VM_DISK_ESP_OFFSET_BYTES) $(INSTALLER_PACKAGE_DIR)/APPS/*.BIN ::/APPS/
	-for f in $(INSTALLER_PACKAGE_DIR)/APPS/*.EXE $(INSTALLER_PACKAGE_DIR)/APPS/*.DLL; do [ -f "$$f" ] && mcopy -i $(VM_DISK)@@$(VM_DISK_ESP_OFFSET_BYTES) "$$f" ::/APPS/; done
	mcopy -i $(VM_DISK)@@$(VM_DISK_ESP_OFFSET_BYTES) \
		$(INSTALLER_PACKAGE_SCRIPT) ::/install.sh
	@echo "VM disk created at: $(VM_DISK)"

vm-disk: $(VM_DISK)

vm-shell: $(VM_DISK) $(OVMF_VARS) check-qemu-tools
	@echo "Starting interactive VM shell for manual testing..."
	@echo "Boot the VM, then use the staged installer on the ESP."
	@echo "Press Ctrl+A followed by X to exit QEMU"
	qemu-system-x86_64 \
		-machine q35 \
		-m 512M \
		$(QEMU_BOOT_FLAGS) \
		$(QEMU_UI_FLAGS) \
		$(QEMU_SERIAL_FLAGS) \
		-drive if=pflash,format=raw,readonly=on,file="$(OVMF_CODE)" \
		-drive if=pflash,format=raw,file="$(OVMF_VARS)" \
		$(QEMU_VM_DISK_FLAGS)

vm-test: $(VM_DISK) $(OVMF_VARS) check-qemu-tools
	@echo "Running automated VM test suite..."
	@echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
	@bash scripts/run-vm-test.sh "$(VM_DISK)" "$(OVMF_CODE)" "$(OVMF_VARS)" 120

fault-test:
	@mkdir -p $(OUT_DIR)
	@$(MAKE) -B FAULT_TEST=1 QEMU_DISPLAY=none QEMU_SERIAL_DEVICE=stdio vm-shell 2>&1 | tee $(OUT_DIR)/fault-test-serial.log

check-vbox-tools:
	@command -v VBoxManage >/dev/null || (echo "Missing VirtualBox: install from virtualbox.org" && exit 1)
	@command -v sgdisk >/dev/null || (echo "Missing sgdisk: brew install gptfdisk" && exit 1)
	@command -v mformat >/dev/null || (echo "Missing mtools: brew install mtools" && exit 1)
	@command -v mmd >/dev/null || (echo "Missing mtools: brew install mtools" && exit 1)
	@command -v mcopy >/dev/null || (echo "Missing mtools: brew install mtools" && exit 1)

$(VBOX_DISK_VDI): installer-package | dirs check-vbox-tools
	@echo "Creating bootable VirtualBox disk image..."
	@rm -f $(VBOX_DISK_RAW) $(VBOX_DISK_VDI)
	dd if=/dev/zero of=$(VBOX_DISK_RAW) bs=1M count=$(VBOX_DISK_SIZE_MB) 2>/dev/null
	sgdisk -n 1:2048:+200M -t 1:EF00 -c 1:EFI $(VBOX_DISK_RAW)
	mformat -i $(VBOX_DISK_RAW)@@$(VBOX_ESP_OFFSET_BYTES) -F ::
	mmd -i $(VBOX_DISK_RAW)@@$(VBOX_ESP_OFFSET_BYTES) ::/EFI ::/EFI/BOOT
	mcopy -i $(VBOX_DISK_RAW)@@$(VBOX_ESP_OFFSET_BYTES) \
		$(INSTALLER_PACKAGE_EFI_DIR)/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i $(VBOX_DISK_RAW)@@$(VBOX_ESP_OFFSET_BYTES) \
		$(INSTALLER_PACKAGE_EFI_DIR)/KERNEL.BIN ::/EFI/BOOT/KERNEL.BIN
	mmd -i $(VBOX_DISK_RAW)@@$(VBOX_ESP_OFFSET_BYTES) ::/APPS
	mcopy -i $(VBOX_DISK_RAW)@@$(VBOX_ESP_OFFSET_BYTES) $(INSTALLER_PACKAGE_DIR)/APPS/*.BIN ::/APPS/
	-for f in $(INSTALLER_PACKAGE_DIR)/APPS/*.EXE $(INSTALLER_PACKAGE_DIR)/APPS/*.DLL; do [ -f "$$f" ] && mcopy -i $(VBOX_DISK_RAW)@@$(VBOX_ESP_OFFSET_BYTES) "$$f" ::/APPS/; done
	VBoxManage convertfromraw $(VBOX_DISK_RAW) $(abspath $(VBOX_DISK_VDI)) --format VDI
	@rm -f $(VBOX_DISK_RAW)
	@echo "VirtualBox disk created at: $(VBOX_DISK_VDI)"

vbox-disk: $(VBOX_DISK_VDI)

vbox-vm-create: $(VBOX_DISK_VDI) check-vbox-tools
	@if VBoxManage list vms | grep -q '"$(VBOX_VM_NAME)"'; then \
		echo "VM '$(VBOX_VM_NAME)' already exists. To recreate: make vbox-vm-delete vbox-vm-create"; \
		exit 0; \
	fi
	VBoxManage createvm --name "$(VBOX_VM_NAME)" --ostype Other_64 --register
	VBoxManage modifyvm "$(VBOX_VM_NAME)" \
		--memory $(VBOX_MEM_MB) \
		--vram 16 \
		--firmware efi64 \
		--graphicscontroller vmsvga \
		--boot1 disk --boot2 none --boot3 none --boot4 none
	VBoxManage storagectl "$(VBOX_VM_NAME)" --name "SATA" --add sata --controller IntelAHCI
	VBoxManage storageattach "$(VBOX_VM_NAME)" \
		--storagectl "SATA" --port 0 --device 0 \
		--type hdd --medium "$(abspath $(VBOX_DISK_VDI))"
	@echo "VM '$(VBOX_VM_NAME)' created. Run: make vbox-run"

vbox-vm-delete: check-vbox-tools
	@VBoxManage list vms | grep -q '"$(VBOX_VM_NAME)"' && \
		(VBoxManage unregistervm "$(VBOX_VM_NAME)" --delete-all && \
		 echo "VM '$(VBOX_VM_NAME)' deleted") || \
		echo "VM '$(VBOX_VM_NAME)' not found"

vbox-run: check-vbox-tools
	@VBoxManage list vms | grep -q '"$(VBOX_VM_NAME)"' || \
		(echo "VM not found. Run: make vbox-vm-create" && exit 1)
	VBoxManage startvm "$(VBOX_VM_NAME)"

clean:
	rm -rf $(OUT_DIR)
