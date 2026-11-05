#include "console.h"
#include "pmm.h"
#include "heap.h"
#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "paging.h"
#include "tcb.h"
#include "scheduler.h"
#include "timer.h"
#include "fb.h"
#include "desktop.h"
#include "usermode.h"
#include "syscall.h"
#include "process.h"

#ifdef KERNEL_TESTS
extern void run_tcb_tests(void);
extern void run_context_switch_tests(void);
extern void run_timer_tests(void);
extern void run_scheduler_tests(void);
extern void run_gdt_tests(void);
extern uint32_t run_stack_guard_tests(void);
#endif

typedef unsigned long long UINT64;
typedef unsigned long long UINTN;
typedef unsigned int UINT32;
typedef unsigned short UINT16;
typedef void *EFI_HANDLE;
typedef UINT64 EFI_STATUS;
typedef UINT64 EFI_PHYSICAL_ADDRESS;

#define EFIAPI __attribute__((ms_abi))
#define EFI_SUCCESS 0
#define EFI_OUT_OF_RESOURCES 0x8000000000000009ULL

typedef enum {
  AllocateAnyPages = 0
} EFI_ALLOCATE_TYPE;

typedef enum {
  EfiLoaderData = 2
} EFI_MEMORY_TYPE;

typedef struct {
  UINT64 Signature;
  UINT32 Revision;
  UINT32 HeaderSize;
  UINT32 CRC32;
  UINT32 Reserved;
} EFI_TABLE_HEADER;

typedef EFI_STATUS(EFIAPI *EFI_ALLOCATE_PAGES)(EFI_ALLOCATE_TYPE Type,
                                               EFI_MEMORY_TYPE MemoryType,
                                               UINTN Pages,
                                               EFI_PHYSICAL_ADDRESS *Memory);

typedef struct EFI_BOOT_SERVICES {
  EFI_TABLE_HEADER Hdr;
  void *RaiseTPL;
  void *RestoreTPL;
  EFI_ALLOCATE_PAGES AllocatePages;
  void *FreePages;
  void *GetMemoryMap;
  void *AllocatePool;
} EFI_BOOT_SERVICES;

typedef struct EFI_SYSTEM_TABLE {
  EFI_TABLE_HEADER Hdr;
  UINT16 *FirmwareVendor;
  UINT32 FirmwareRevision;
  EFI_HANDLE ConsoleInHandle;
  void *ConIn;
  EFI_HANDLE ConsoleOutHandle;
  void *ConOut;
  EFI_HANDLE StandardErrorHandle;
  void *StdErr;
  void *RuntimeServices;
  EFI_BOOT_SERVICES *BootServices;
} EFI_SYSTEM_TABLE;

typedef struct {
  unsigned char Magic[8];
  unsigned int AbiVersion;
  unsigned int HeaderSize;
  unsigned long long EntryOffset;
  unsigned long long ImageSize;
  unsigned long long Flags;
  unsigned long long BssSize;   // extra zero-fill bytes after the binary image
} KERNEL_IMAGE_HEADER;

typedef struct {
  unsigned long long Magic;
  unsigned int Version;
  unsigned int Size;
  unsigned long long KernelImageBase;
  unsigned long long KernelImageSize;
  void *SystemTable;
  // Physical memory map from UEFI GetMemoryMap
  EFI_MEMORY_DESCRIPTOR *MemoryMap;
  unsigned long long MemoryMapSize;
  unsigned long long MemoryMapDescSize;
  unsigned long long MemoryMapEntries;
  // Linear framebuffer from the UEFI Graphics Output Protocol (0 = none)
  unsigned long long FramebufferBase;
  unsigned long long FramebufferSize;
  unsigned int FramebufferWidth;
  unsigned int FramebufferHeight;
  unsigned int FramebufferStride;
  unsigned int FramebufferFormat;
} KERNEL_BOOT_INFO;

typedef struct DemoTaskState {
  const char *name;
  unsigned int completed_phases;
  uint64_t work_units;
} DemoTaskState;

static int demo_task(void *arg) {
  DemoTaskState *state = (DemoTaskState *)arg;
  uint64_t start_ticks;
  uint64_t local_work = 0;

  if (state == 0) {
    return 0;
  }

  start_ticks = timer_get_ticks();
  while ((timer_get_ticks() - start_ticks) < 4) {
    local_work++;
    __asm__ volatile("pause");
  }

  state->work_units += local_work;
  console_write_string("        ");
  console_write_string(state->name);
  console_write_string(" phase ");
  console_printf("%u", state->completed_phases);
  console_write_string(" complete\r\n");

  state->completed_phases++;
  return state->completed_phases < 2;
}

static __attribute__((noreturn)) void kernel_runtime(const KERNEL_BOOT_INFO *boot_info) {
  (void)boot_info;
#ifdef KERNEL_FAULT_TEST
  console_write_string("[TEST] Triggering deliberate invalid-opcode trap\r\n");
  __asm__ volatile("ud2");
#endif
  console_write_string("[5] Initializing kernel heap (4 MB)...\r\n");
  heap_init();
  console_write_string("    Heap initialized\r\n");

  void *a = kmalloc(64);
  void *b = kmalloc(128);
  kfree(a);
  void *c = kmalloc(32);
  (void)a;
  (void)b;
  (void)c;
  console_write_string("    kmalloc smoke test completed\r\n");
  kfree(b);
  kfree(c);
  console_write_string("    kfree smoke test completed\r\n");

  console_newline();
  console_write_string("[6] Initializing Task Control Block pool\r\n");
  tcb_init();
  console_write_string("    TCB pool initialized (256 max tasks)\r\n");

  syscall_init();
  console_write_string("    SYSCALL/SYSRET enabled\r\n");

#if defined(KERNEL_USER_SELFTEST) && KERNEL_USER_SELFTEST == 3
  process_selftest();
#elif defined(KERNEL_USER_SELFTEST) && (KERNEL_USER_SELFTEST == 1 || KERNEL_USER_SELFTEST == 2)
  usermode_selftest();
#endif

#ifdef KERNEL_TESTS
  console_newline();
  console_write_string("[6.5] Running TCB test suite\r\n");
  run_tcb_tests();

  console_newline();
  console_write_string("[6.5.1] Running stack guard test suite\r\n");
  run_stack_guard_tests();

  console_newline();
  console_write_string("[6.6] Running context switch test suite\r\n");
  run_context_switch_tests();

  console_newline();
  console_write_string("[6.6.5] Running GDT test suite\r\n");
  run_gdt_tests();

  console_newline();
  console_write_string("[6.7] Running timer test suite\r\n");
  run_timer_tests();

  console_newline();
  console_write_string("[6.8] Running scheduler test suite\r\n");
  run_scheduler_tests();
#else
  console_newline();
  console_write_string("[6.5] Initializing timer\r\n");
  timer_init();
  timer_unmask_irq0();
#endif

  console_newline();
  console_write_string("[7] Setting up scheduler\r\n");

  {
    DemoTaskState task_a = {"task-a", 0, 0};
    DemoTaskState task_b = {"task-b", 0, 0};

    scheduler_init();
    scheduler_set_idle_callback(desktop_start);
#if defined(KERNEL_USER_SELFTEST) && KERNEL_USER_SELFTEST == 4
    process_scheduler_selftest();
#elif defined(KERNEL_USER_SELFTEST) && KERNEL_USER_SELFTEST == 5
    process_desktop_selftest();
#elif defined(KERNEL_USER_SELFTEST) && KERNEL_USER_SELFTEST == 6
    process_isolation_selftest();
#endif
    scheduler_add_task("task-a", demo_task, &task_a);
    scheduler_add_task("task-b", demo_task, &task_b);
    scheduler_run();
  }
}

// Kernel entry point (called from bootloader)
__attribute__((noreturn)) void kernel_entry(const KERNEL_BOOT_INFO *boot_info) {
  EFI_SYSTEM_TABLE *system_table;
  EFI_BOOT_SERVICES *boot_services;
  EFI_PHYSICAL_ADDRESS page_table_pool_phys;
  void *page_table_pool;
  EFI_STATUS status;

  // Initialize console with UEFI system table
  console_init((void *)boot_info->SystemTable);
  system_table = (EFI_SYSTEM_TABLE *)boot_info->SystemTable;
  boot_services = (system_table != 0) ? system_table->BootServices : 0;

  page_table_pool = 0;
  page_table_pool_phys = 0;
  if (boot_services == 0 || boot_services->AllocatePages == 0) {
    console_write_string("Boot services unavailable for paging pool\r\n");
    __builtin_unreachable();
  }

  status = boot_services->AllocatePages(AllocateAnyPages,
                                        EfiLoaderData,
                                        64,
                                        &page_table_pool_phys);
  if (status != EFI_SUCCESS || page_table_pool_phys == 0) {
    console_write_string("Failed to allocate paging pool\r\n");
    __builtin_unreachable();
  }
  page_table_pool = (void *)(uintptr_t)page_table_pool_phys;
  paging_set_page_table_pool(page_table_pool, 64ULL * 4096ULL);

  console_write_string("================================\r\n");
  console_write_string("WinClone Kernel\r\n");
  console_write_string("================================\r\n");
  console_newline();
  idt_init();
  pic_init();
  console_write_string("[0] Exception handlers installed\r\n");

  // Verify boot info
  console_write_string("Boot Info:\r\n");
  console_write_string("  Magic: ");
  if (boot_info->Magic == 0x31544F4F424C4357ULL) {
    console_write_string("OK\r\n");
  } else {
    console_write_string("INVALID\r\n");
  }

  console_printf("  Kernel Base: 0x%llx\r\n", boot_info->KernelImageBase);
  console_printf("  Kernel Size: 0x%llx bytes\r\n", boot_info->KernelImageSize);
  console_newline();

  // [1] Console
  console_write_string("[1] Console initialized\r\n");

  // [2] Physical memory manager
  console_write_string("[2] Initializing physical memory manager...\r\n");
  pmm_init(boot_info->MemoryMap,
           boot_info->MemoryMapSize,
           boot_info->MemoryMapDescSize,
           boot_info->MemoryMapEntries);
  console_write_string("    PMM initialized\r\n");
  console_write_string("    PMM free page count available\r\n");

  // [3] Paging
  console_write_string("[3] Initializing kernel paging...\r\n");
  if (paging_init(boot_info->MemoryMap,
                  boot_info->KernelImageBase,
                  boot_info->KernelImageSize,
                  boot_info->MemoryMapDescSize,
                  boot_info->MemoryMapEntries)) {
    uint16_t kernel_cs;
    console_write_string("    Paging active\r\n");
    // The UEFI GDT is at an unmapped physical address now that our page
    // tables are active.  Install a minimal kernel GDT (in mapped BSS) and
    // update every IDT gate to use its code-segment selector so exception
    // delivery can succeed.
    kernel_cs = gdt_init();
    idt_reload_selector(kernel_cs);
    console_write_string("    Kernel GDT installed\r\n");

    if (boot_info->FramebufferBase != 0) {
      if (fb_init(boot_info->FramebufferBase,
                  boot_info->FramebufferSize,
                  boot_info->FramebufferWidth,
                  boot_info->FramebufferHeight,
                  boot_info->FramebufferStride,
                  boot_info->FramebufferFormat)) {
        console_printf("    Framebuffer: %ux%u at 0x%llx\r\n",
                       boot_info->FramebufferWidth,
                       boot_info->FramebufferHeight,
                       boot_info->FramebufferBase);
      } else {
        console_write_string("    Framebuffer: init failed\r\n");
      }
    } else {
      console_write_string("    Framebuffer: none\r\n");
    }
  } else {
    console_write_string("    Paging setup failed\r\n");
    for (;;) {
      __asm__ volatile("pause");
    }
  }

  {
    uintptr_t kernel_base = (uintptr_t)boot_info->KernelImageBase;
    uintptr_t kernel_base_page = kernel_base & ~(uintptr_t)0xFFFULL;
    uintptr_t runtime_offset = (uintptr_t)kernel_runtime - kernel_base_page;
    void (*runtime_entry)(const KERNEL_BOOT_INFO *) =
        (void (*)(const KERNEL_BOOT_INFO *))
        (uintptr_t)(KERNEL_HIGH_HALF_BASE + runtime_offset);

    console_write_string("[4] Switching to higher-half kernel\r\n");
    runtime_entry(boot_info);
    __builtin_unreachable();
  }
}
