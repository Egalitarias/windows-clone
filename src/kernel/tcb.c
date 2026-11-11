#include <stddef.h>
#include "tcb.h"
#include "heap.h"
#include "paging.h"
#include "pmm.h"
#include "string.h"
#include "stack_guard.h"

static TCB g_tcb_pool[TCB_MAX_TASKS];
static uint32_t g_next_task_id = 1;
static uint32_t g_active_tasks = 0;

static void tcb_free_slot(TCB *tcb);

void tcb_init(void) {
  uint32_t i;

  for (i = 0; i < TCB_MAX_TASKS; ++i) {
    if (g_tcb_pool[i].task_id != 0) {
      tcb_free_slot(&g_tcb_pool[i]);
    }
    g_tcb_pool[i].task_id = 0;
    g_tcb_pool[i].name = 0;
    g_tcb_pool[i].stack_pointer = 0;
    g_tcb_pool[i].instruction_pointer = 0;
    g_tcb_pool[i].register_state = 0;
    g_tcb_pool[i].state = TASK_STATE_TERMINATED;
    g_tcb_pool[i].priority = 127;
    g_tcb_pool[i].stack_base = 0;
    g_tcb_pool[i].stack_size = 0;
    g_tcb_pool[i].ticks_running = 0;
    g_tcb_pool[i].context_switches = 0;
    g_tcb_pool[i].guard_page_low = 0;
    g_tcb_pool[i].guard_page_high = 0;
    g_tcb_pool[i].overflow_attempts = 0;
    g_tcb_pool[i].address_space = NULL;
    g_tcb_pool[i].user_stack_base = NULL;
    g_tcb_pool[i].user_entry_point = NULL;
    g_tcb_pool[i].exit_code = 0;
  }
  g_next_task_id = 1;
  g_active_tasks = 0;
}

static int tcb_find_free_slot(void) {
  uint32_t i;

  for (i = 0; i < TCB_MAX_TASKS; ++i) {
    if (g_tcb_pool[i].task_id == 0) {
      return (int)i;
    }
  }
  return -1;
}

static TCB *tcb_slot_by_index(uint32_t index) {
  if (index >= TCB_MAX_TASKS) {
    return 0;
  }
  return &g_tcb_pool[index];
}

static void tcb_free_slot(TCB *tcb) {
  uint64_t stack_phys_base;

  if (tcb == 0) {
    return;
  }
  tcb->task_id = 0;
  tcb->name = 0;
  if (tcb->stack_base != 0) {
    stack_phys_base = (uint64_t)(uintptr_t)tcb->stack_base;
    paging_unmap_identity_range(stack_phys_base, STACK_GUARD_PAGE_SIZE);
    pmm_free_page(stack_phys_base - STACK_GUARD_PAGE_SIZE);
    pmm_free_page(stack_phys_base);
    pmm_free_page(stack_phys_base + STACK_GUARD_PAGE_SIZE);
    tcb->stack_base = 0;
  }
  tcb->guard_page_low = 0;
  tcb->guard_page_high = 0;
  tcb->overflow_attempts = 0;
  if (tcb->register_state != 0) {
    kfree(tcb->register_state);
    tcb->register_state = 0;
  }
  tcb->stack_pointer = 0;
  tcb->instruction_pointer = 0;
  tcb->state = TASK_STATE_TERMINATED;
  tcb->stack_size = 0;
  tcb->address_space = 0;
  tcb->cr3 = 0;
  tcb->kernel_rsp0 = 0;
  tcb->gs_base = 0;
  memset(&tcb->uctx, 0, sizeof(tcb->uctx));
  tcb->thread = 0;
  tcb->wait_kind = 0;
  tcb->wake_ms = 0;
  tcb->user_stack_base = 0;
  tcb->user_entry_point = 0;
  tcb->exit_code = 0;
}

static void tcb_allocate_stack(TCB *tcb) {
  uint64_t stack_region_phys;
  uint8_t *stack;
  uint8_t *stack_top;

  if (tcb == 0) {
    return;
  }

  /*
   * Allocate 3 contiguous physical pages and map only the middle page:
   * [guard-low: not present][stack: present][guard-high: not present]
   */
  stack_region_phys = pmm_alloc_pages(3);
  if (stack_region_phys == 0) {
    return;
  }

  if (!paging_map_range(stack_region_phys + STACK_GUARD_PAGE_SIZE, STACK_GUARD_PAGE_SIZE)) {
    pmm_free_page(stack_region_phys);
    pmm_free_page(stack_region_phys + STACK_GUARD_PAGE_SIZE);
    pmm_free_page(stack_region_phys + (2ULL * STACK_GUARD_PAGE_SIZE));
    return;
  }

  stack = (uint8_t *)(uintptr_t)(stack_region_phys + STACK_GUARD_PAGE_SIZE);

  stack_top = stack + TCB_DEFAULT_STACK_SIZE;

  tcb->stack_base = (void *)stack;
  tcb->stack_size = TCB_DEFAULT_STACK_SIZE;
  tcb->stack_pointer = (void *)(stack_top - sizeof(uint64_t));
  tcb->guard_page_low = (void *)(uintptr_t)stack_region_phys;
  tcb->guard_page_high = (void *)(uintptr_t)(stack_region_phys + (2ULL * STACK_GUARD_PAGE_SIZE));
  tcb->overflow_attempts = 0;

  // Write watermarks at stack boundaries for overflow detection.
  *(uint64_t *)stack = STACK_GUARD_WATERMARK_LOW;
  *(uint64_t *)((uintptr_t)stack + TCB_DEFAULT_STACK_SIZE - sizeof(uint64_t)) =
      STACK_GUARD_WATERMARK_HIGH;

  tcb->register_state = (uint64_t *)kmalloc(17 * sizeof(uint64_t));
  if (tcb->register_state == 0) {
    paging_unmap_identity_range((uint64_t)(uintptr_t)stack, STACK_GUARD_PAGE_SIZE);
    pmm_free_page(stack_region_phys);
    pmm_free_page(stack_region_phys + STACK_GUARD_PAGE_SIZE);
    pmm_free_page(stack_region_phys + (2ULL * STACK_GUARD_PAGE_SIZE));
    tcb->stack_base = 0;
    tcb->guard_page_low = 0;
    tcb->guard_page_high = 0;
    tcb->stack_pointer = 0;
    tcb->stack_size = 0;
    return;
  }

  int i;
  for (i = 0; i < 17; ++i) {
    tcb->register_state[i] = 0;
  }
  tcb->register_state[14] = (uint64_t)(uintptr_t)tcb->stack_pointer;
}

TCB *tcb_create(const char *name, uint32_t priority) {
  int slot;
  TCB *tcb;

  if (g_active_tasks >= TCB_MAX_TASKS) {
    return 0;
  }

  slot = tcb_find_free_slot();
  if (slot < 0) {
    return 0;
  }

  tcb = tcb_slot_by_index((uint32_t)slot);
  if (tcb == 0) {
    return 0;
  }

  tcb->task_id = g_next_task_id++;
  tcb->name = name;
  tcb->state = TASK_STATE_READY;
  tcb->priority = priority;
  tcb->ticks_running = 0;
  tcb->context_switches = 0;
  {
    int i;

    for (i = 0; i < 512; ++i) {                  // a clean x87/SSE state: default control words
      tcb->fx_state[i] = 0;
    }
    tcb->fx_state[0] = 0x7F;                       // FCW = 0x037F
    tcb->fx_state[1] = 0x03;
    tcb->fx_state[24] = 0x80;                      // MXCSR = 0x1F80 (all exceptions masked)
    tcb->fx_state[25] = 0x1F;
  }

  tcb_allocate_stack(tcb);

  if (tcb->stack_base == 0) {
    tcb->task_id = 0;
    return 0;
  }

  ++g_active_tasks;
  return tcb;
}

void tcb_destroy(TCB *tcb) {
  if (tcb == 0 || tcb->task_id == 0) {
    return;
  }

  if ((uint8_t *)tcb < (uint8_t *)g_tcb_pool || (uint8_t *)tcb >= (uint8_t *)(g_tcb_pool + TCB_MAX_TASKS)) {
    return;
  }

  tcb_free_slot(tcb);
  if (g_active_tasks > 0) {
    --g_active_tasks;
  }
}

TCB *tcb_get_by_id(uint32_t task_id) {
  uint32_t i;

  if (task_id == 0) {
    return 0;
  }

  for (i = 0; i < TCB_MAX_TASKS; ++i) {
    if (g_tcb_pool[i].task_id == task_id) {
      return &g_tcb_pool[i];
    }
  }
  return 0;
}

TCB *tcb_get_by_index(uint32_t index) {
  if (index >= TCB_MAX_TASKS) {
    return 0;
  }
  return &g_tcb_pool[index];
}
