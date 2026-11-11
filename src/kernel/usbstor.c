#include "usbstor.h"
#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "scheduler.h"
#include "string.h"
#include "timer.h"

// xHCI host controller + USB mass storage (Bulk-Only Transport, SCSI transparent
// command set). Everything is polled: one command or transfer in flight, one event
// ring, memory from the PMM (identity mapped, so physical == virtual). Devices must be
// plugged into the controller's own ports (no hubs). Only 512-byte-block devices
// are exposed.

#define XHCI_MAX_CONTROLLERS 2
#define XHCI_MAX_SLOTS       16
#define RING_TRBS            64            // the last one is the Link TRB
#define EVENT_TRBS           64

// Capability registers
#define CAP_CAPLENGTH   0x00               // byte
#define CAP_HCSPARAMS1  0x04
#define CAP_HCSPARAMS2  0x08
#define CAP_HCCPARAMS1  0x10
#define CAP_DBOFF       0x14
#define CAP_RTSOFF      0x18
// Operational registers (from CAPLENGTH)
#define OP_USBCMD   0x00
#define OP_USBSTS   0x04
#define OP_PAGESIZE 0x08
#define OP_CRCR     0x18
#define OP_DCBAAP   0x30
#define OP_CONFIG   0x38
#define OP_PORTSC   0x400                  // + 0x10 * (port - 1)

#define CMD_RS      (1u << 0)
#define CMD_HCRST   (1u << 1)
#define STS_HCH     (1u << 0)
#define STS_CNR     (1u << 11)

#define PORT_CCS    (1u << 0)
#define PORT_PED    (1u << 1)
#define PORT_PR     (1u << 4)
#define PORT_PP     (1u << 9)
#define PORT_PRC    (1u << 21)
#define PORT_CHANGE_BITS 0x00FE0000u       // CSC..CEC, write 1 to clear

// TRB types
#define TRB_NORMAL        1
#define TRB_SETUP         2
#define TRB_DATA          3
#define TRB_STATUS        4
#define TRB_LINK          6
#define TRB_CMD_ENABLE    9
#define TRB_CMD_ADDRESS   11
#define TRB_CMD_CONFIGURE 12
#define TRB_CMD_EVALUATE  13
#define TRB_CMD_RESET_EP  14
#define TRB_CMD_SET_DEQ   16
#define TRB_EV_TRANSFER   32
#define TRB_EV_COMMAND    33

#define CC_SUCCESS  1
#define CC_STALL    6
#define CC_SHORT    13

#define TIMEOUT_MS  3000

typedef struct {
  volatile uint32_t d[4];
} trb_t;

typedef struct {
  trb_t *trb;
  uint64_t phys;
  uint32_t enq;
  uint32_t cycle;
} ring_t;

typedef struct {
  int active;                // polling this endpoint
  int kind;                  // 1 keyboard, 2 mouse
  uint8_t ep, iface, interval;
  uint16_t mps;
  uint32_t reqlen;           // bytes asked for per report (at most 8 in boot protocol)
  uint16_t desc_len;         // length of the HID report descriptor (from the HID descriptor)
  int report_mode;           // mouse in report protocol: fields found in the report descriptor
  int has_id;                // reports start with a report ID byte
  uint8_t rid;
  uint32_t rep_len;          // bytes in one report (with the ID)
  int btn_off, x_off, y_off, w_off;   // bit offsets of buttons 1..3, X, Y, wheel
  int x_sz, y_sz, w_sz;      // field sizes in bits
  uint32_t dci;
  ring_t ring;
  uint8_t *buf;
  uint64_t buf_phys;
} hid_t;

typedef struct {
  int used;
  int slot;
  int speed;
  int port;                  // root-hub port the device hangs off (through any hubs)
  uint32_t route;            // route string: 4 bits per hub tier, first hub in the low nibble
  int depth;                 // number of hubs between this device and the root port
  int dev_class;             // bDeviceClass (9 = hub)
  int tt_slot, tt_port;      // low/full-speed device behind a high-speed hub: its transaction translator
  int is_hub;
  int hub_ports;             // downstream ports (hubs only)
  uint32_t hub_ttt;          // think time field of the hub's slot context
  uint32_t hub_pwr_ms;       // power-on to power-good
  uint8_t *ctx;              // output device context
  uint64_t ctx_phys;
  ring_t ep0;
  ring_t bulk_in, bulk_out;
  uint8_t in_ep, out_ep;     // endpoint addresses (bit 7 = IN)
  uint16_t in_mps, out_mps;
  uint8_t cfg_value, iface;
  uint32_t tag;
  int is_storage;
  int has_storage;           // found a mass-storage interface
  hid_t hid[2];              // boot keyboard / mouse interfaces
  int hid_count;
} xdev_t;

typedef struct {
  uintptr_t base;
  uintptr_t op, rt, db;
  uint32_t ctx_size;         // 32 or 64
  uint32_t max_ports, max_slots;
  uint64_t *dcbaa;
  uint64_t dcbaa_phys;
  ring_t cmd;
  trb_t *ev;
  uint64_t ev_phys;
  uint32_t ev_deq, ev_cycle;
  uint8_t *dbuf;             // control transfer data (one page)
  uint64_t dbuf_phys;
  uint8_t *bounce;           // 64 KiB, aligned so a TRB never crosses a 64 KiB boundary
  uint64_t bounce_phys;
  uint8_t *input;            // input context (one page)
  uint64_t input_phys;
  xdev_t dev[XHCI_MAX_SLOTS + 1];
  int ready;
} xhci_t;

static xhci_t g_x[XHCI_MAX_CONTROLLERS];
static int g_x_count = 0;
static usb_disk_t g_disks[USB_MAX_DISKS];
static int g_disk_count = 0;
static int g_hid_active = 0;           // keyboard / mouse endpoints being polled

// ---- MMIO and small helpers --------------------------------------------------

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr64(uintptr_t a, uint64_t v) {
  wr32(a, (uint32_t)(v & 0xFFFFFFFFu));
  wr32(a + 4, (uint32_t)(v >> 32));
}

static void delay_ms(uint32_t ms) {
  uint64_t start = timer_get_ms();
  while (timer_get_ms() - start < ms) {
    __asm__ volatile("pause");
  }
}

static uint64_t alloc_mapped(uint32_t pages) {
  uint64_t phys = pmm_alloc_pages(pages);
  if (phys == 0 || !paging_map_range(phys, (uint64_t)pages * 4096u)) {
    return 0;
  }
  memset((void *)(uintptr_t)phys, 0, (size_t)pages * 4096u);
  return phys;
}

static int ring_init(ring_t *r) {
  r->phys = alloc_mapped(1);
  if (r->phys == 0) {
    return 0;
  }
  r->trb = (trb_t *)(uintptr_t)r->phys;
  r->enq = 0;
  r->cycle = 1;
  return 1;
}

// Queue one TRB (control = everything except the cycle bit). Returns its physical address.
static uint64_t ring_push(ring_t *r, uint64_t param, uint32_t status, uint32_t control) {
  trb_t *t = &r->trb[r->enq];
  uint64_t where = r->phys + (uint64_t)r->enq * sizeof(trb_t);

  t->d[0] = (uint32_t)(param & 0xFFFFFFFFu);
  t->d[1] = (uint32_t)(param >> 32);
  t->d[2] = status;
  __asm__ volatile("" : : : "memory");
  t->d[3] = control | r->cycle;
  r->enq++;
  if (r->enq == RING_TRBS - 1) {                     // hand over the Link TRB and wrap
    trb_t *l = &r->trb[r->enq];
    l->d[0] = (uint32_t)(r->phys & 0xFFFFFFFFu);
    l->d[1] = (uint32_t)(r->phys >> 32);
    l->d[2] = 0;
    __asm__ volatile("" : : : "memory");
    l->d[3] = (TRB_LINK << 10) | (1u << 1) | r->cycle;     // toggle cycle
    r->enq = 0;
    r->cycle ^= 1u;
  }
  return where;
}

static void ring_doorbell(const xhci_t *x, uint32_t slot, uint32_t target) {
  __asm__ volatile("" : : : "memory");
  wr32(x->db + 4u * slot, target);
}

// Fetch the next event if one is pending (non-blocking). Returns 1 and fills out[4].
static int event_next(xhci_t *x, uint32_t out[4]) {
  trb_t *t = &x->ev[x->ev_deq];
  uint32_t c = t->d[3];
  uint64_t erdp;

  if ((c & 1u) != x->ev_cycle) {
    return 0;
  }
  out[0] = t->d[0];
  out[1] = t->d[1];
  out[2] = t->d[2];
  out[3] = c;
  x->ev_deq++;
  if (x->ev_deq == EVENT_TRBS) {
    x->ev_deq = 0;
    x->ev_cycle ^= 1u;
  }
  erdp = x->ev_phys + (uint64_t)x->ev_deq * sizeof(trb_t);
  wr64(x->rt + 0x20 + 0x18, erdp | (1u << 3));         // interrupter 0 ERDP, clear EHB
  return 1;
}

static void hid_event(xhci_t *x, const uint32_t ev[4]);

// Wait for an event of `type` (for transfer events also matching slot and endpoint id,
// ep_id 0 = any). Reports of keyboards and mice that arrive meanwhile are handled;
// other events are discarded. Returns 1 and fills out[4].
static int event_wait(xhci_t *x, uint32_t type, uint32_t slot, uint32_t ep_id, uint32_t out[4]) {
  uint64_t start = timer_get_ms();

  for (;;) {
    uint32_t ev[4];

    if (event_next(x, ev)) {
      uint32_t et = (ev[3] >> 10) & 0x3Fu;

      if (et == type && (type == TRB_EV_COMMAND ||
                         ((ev[3] >> 24) == slot && (ep_id == 0 || ((ev[3] >> 16) & 0x1Fu) == ep_id)))) {
        out[0] = ev[0]; out[1] = ev[1]; out[2] = ev[2]; out[3] = ev[3];
        return 1;
      }
      if (et == TRB_EV_TRANSFER) {
        hid_event(x, ev);
      }
      continue;
    }
    if (timer_get_ms() - start > TIMEOUT_MS) {
      return 0;
    }
  }
}

// Run one command TRB. Returns the completion code (0 on timeout); *slot_out gets the slot id.
static uint32_t command(xhci_t *x, uint64_t param, uint32_t control, uint32_t *slot_out) {
  uint32_t ev[4];

  ring_push(&x->cmd, param, 0, control);
  ring_doorbell(x, 0, 0);
  if (!event_wait(x, TRB_EV_COMMAND, 0, 0, ev)) {
    return 0;
  }
  if (slot_out) {
    *slot_out = ev[3] >> 24;
  }
  return ev[2] >> 24;
}

// ---- Controller bring-up -----------------------------------------------------

static int wait_op(const xhci_t *x, uint32_t off, uint32_t mask, uint32_t want, uint32_t ms) {
  uint64_t start = timer_get_ms();
  while ((rd32(x->op + off) & mask) != want) {
    if (timer_get_ms() - start > ms) {
      return 0;
    }
  }
  return 1;
}

// Take the controller over from the firmware (USB Legacy Support capability).
static void bios_handoff(const xhci_t *x) {
  uint32_t hccparams1 = rd32(x->base + CAP_HCCPARAMS1);
  uint32_t off = (hccparams1 >> 16) << 2;
  int guard = 0;

  while (off != 0 && guard++ < 32) {
    uintptr_t cap = x->base + off;
    uint32_t v = rd32(cap);

    if ((v & 0xFFu) == 1) {                           // USB Legacy Support
      uint64_t start = timer_get_ms();
      wr32(cap, v | (1u << 24));                      // OS owned semaphore
      while ((rd32(cap) & (1u << 16)) && timer_get_ms() - start < 1000) {
      }
      wr32(cap + 4, 0);                               // no SMIs from the controller any more
      return;
    }
    if (((v >> 8) & 0xFFu) == 0) {
      return;
    }
    off += ((v >> 8) & 0xFFu) << 2;
  }
}

static int controller_start(xhci_t *x, const pci_dev_t *dev) {
  uint32_t bar0, bar1, hcs1, hcs2, scratch, i;
  uint64_t bar, erst_phys;
  uint32_t *erst;

  memset(x, 0, sizeof(*x));
  pci_enable_device(dev);
  bar0 = pci_read32(dev->bus, dev->dev, dev->fn, 0x10);
  bar1 = pci_read32(dev->bus, dev->dev, dev->fn, 0x14);
  if (bar0 & 1u) {
    return 0;
  }
  bar = (uint64_t)(bar0 & 0xFFFFFFF0u);
  if (((bar0 >> 1) & 3u) == 2u) {
    bar |= (uint64_t)bar1 << 32;
  }
  if (bar == 0 || !paging_map_mmio(bar, 0x10000)) {
    return 0;
  }
  x->base = (uintptr_t)bar;
  x->op = x->base + (rd32(x->base) & 0xFFu);
  x->db = x->base + (rd32(x->base + CAP_DBOFF) & ~3u);
  x->rt = x->base + (rd32(x->base + CAP_RTSOFF) & ~0x1Fu);
  hcs1 = rd32(x->base + CAP_HCSPARAMS1);
  hcs2 = rd32(x->base + CAP_HCSPARAMS2);
  x->max_slots = hcs1 & 0xFFu;
  x->max_ports = hcs1 >> 24;
  x->ctx_size = (rd32(x->base + CAP_HCCPARAMS1) & (1u << 2)) ? 64u : 32u;
  if (x->max_slots > XHCI_MAX_SLOTS) {
    x->max_slots = XHCI_MAX_SLOTS;
  }
  if (x->max_ports > 32) {
    x->max_ports = 32;
  }

  bios_handoff(x);

  // Stop, then reset.
  wr32(x->op + OP_USBCMD, rd32(x->op + OP_USBCMD) & ~CMD_RS);
  if (!wait_op(x, OP_USBSTS, STS_HCH, STS_HCH, 1000)) {
    return 0;
  }
  wr32(x->op + OP_USBCMD, CMD_HCRST);
  if (!wait_op(x, OP_USBCMD, CMD_HCRST, 0, 1000) || !wait_op(x, OP_USBSTS, STS_CNR, 0, 1000)) {
    return 0;
  }

  x->dcbaa_phys = alloc_mapped(1);
  x->dbuf_phys = alloc_mapped(1);
  x->input_phys = alloc_mapped(1);
  erst_phys = alloc_mapped(1);
  {
    uint64_t raw = alloc_mapped(32);                    // 128 KiB: a 64 KiB window inside
    uint64_t ev_phys = alloc_mapped(1);

    if (!x->dcbaa_phys || !x->dbuf_phys || !x->input_phys || !erst_phys || !raw || !ev_phys) {
      return 0;
    }
    x->bounce_phys = (raw + 0xFFFFu) & ~0xFFFFull;
    x->ev_phys = ev_phys;
  }
  x->dcbaa = (uint64_t *)(uintptr_t)x->dcbaa_phys;
  x->dbuf = (uint8_t *)(uintptr_t)x->dbuf_phys;
  x->input = (uint8_t *)(uintptr_t)x->input_phys;
  x->bounce = (uint8_t *)(uintptr_t)x->bounce_phys;
  x->ev = (trb_t *)(uintptr_t)x->ev_phys;
  x->ev_cycle = 1;
  if (!ring_init(&x->cmd)) {
    return 0;
  }

  // Scratchpad buffers the controller wants for itself.
  scratch = ((hcs2 >> 21) & 0x1Fu) << 5 | ((hcs2 >> 27) & 0x1Fu);
  if (scratch > 0) {
    uint64_t arr_phys = alloc_mapped(1);
    uint64_t *arr = (uint64_t *)(uintptr_t)arr_phys;

    if (!arr_phys || scratch > 512) {
      return 0;
    }
    for (i = 0; i < scratch; i++) {
      uint64_t p = alloc_mapped(1);
      if (!p) {
        return 0;
      }
      arr[i] = p;
    }
    x->dcbaa[0] = arr_phys;
  }

  wr32(x->op + OP_CONFIG, x->max_slots);
  wr64(x->op + OP_DCBAAP, x->dcbaa_phys);
  wr64(x->op + OP_CRCR, x->cmd.phys | 1u);

  erst = (uint32_t *)(uintptr_t)erst_phys;               // one segment
  erst[0] = (uint32_t)(x->ev_phys & 0xFFFFFFFFu);
  erst[1] = (uint32_t)(x->ev_phys >> 32);
  erst[2] = EVENT_TRBS;
  erst[3] = 0;
  wr32(x->rt + 0x20 + 0x08, 1);                          // ERSTSZ
  wr64(x->rt + 0x20 + 0x18, x->ev_phys);                 // ERDP
  wr64(x->rt + 0x20 + 0x10, erst_phys);                  // ERSTBA (last: starts the ring)

  wr32(x->op + OP_USBCMD, CMD_RS);
  if (!wait_op(x, OP_USBSTS, STS_HCH, 0, 1000)) {
    return 0;
  }
  return 1;
}

// ---- Slot and endpoint contexts ----------------------------------------------

static uint32_t *ictx_ptr(xhci_t *x, uint32_t index) {
  return (uint32_t *)(x->input + (size_t)index * x->ctx_size);
}

static void input_clear(xhci_t *x) {
  memset(x->input, 0, 4096);
}

static void ep_context(xhci_t *x, uint32_t dci, uint32_t type, uint32_t mps, const ring_t *r, uint32_t avg) {
  uint32_t *e = ictx_ptr(x, dci + 1);

  e[1] = (3u << 1) | (type << 3) | (mps << 16);
  e[2] = (uint32_t)((r->phys & 0xFFFFFFFFu) | 1u);       // dequeue pointer, DCS = 1
  e[3] = (uint32_t)(r->phys >> 32);
  e[4] = avg | (avg << 16);
}

// Slot context for the device: route string, speed, root port, hub fields, transaction translator.
static void fill_slot(const xdev_t *d, uint32_t *s, uint32_t entries) {
  s[0] = d->route | ((uint32_t)d->speed << 20) | (d->is_hub ? (1u << 26) : 0u) | (entries << 27);
  s[1] = ((uint32_t)d->port << 16) | (d->is_hub ? ((uint32_t)d->hub_ports << 24) : 0u);
  s[2] = (uint32_t)d->tt_slot | ((uint32_t)d->tt_port << 8) | (d->is_hub ? (d->hub_ttt << 16) : 0u);
}

static int speed_mps0(int speed) {
  return speed == 4 ? 512 : (speed == 2 ? 8 : 64);        // super / low / full+high
}

// ---- Control transfers on endpoint 0 -----------------------------------------

static int control(xhci_t *x, xdev_t *d, uint8_t rtype, uint8_t req, uint16_t value, uint16_t index,
                   uint16_t len, int in) {
  uint32_t ev[4];
  uint32_t trt = len == 0 ? 0u : (in ? 3u : 2u);

  ring_push(&d->ep0, (uint64_t)rtype | ((uint64_t)req << 8) | ((uint64_t)value << 16) |
                         ((uint64_t)index << 32) | ((uint64_t)len << 48),
            8, (TRB_SETUP << 10) | (1u << 6) | (trt << 16));
  if (len != 0) {
    ring_push(&d->ep0, x->dbuf_phys, len, (TRB_DATA << 10) | (in ? (1u << 16) : 0u));
  }
  ring_push(&d->ep0, 0, 0, (TRB_STATUS << 10) | (1u << 5) | ((len == 0 || !in) ? (1u << 16) : 0u));
  ring_doorbell(x, (uint32_t)d->slot, 1);
  if (!event_wait(x, TRB_EV_TRANSFER, (uint32_t)d->slot, 1, ev)) {
    return 0;
  }
  return (ev[2] >> 24) == CC_SUCCESS || (ev[2] >> 24) == CC_SHORT;
}

// ---- Device enumeration ------------------------------------------------------

static int reset_port(xhci_t *x, uint32_t port, int *speed) {
  uintptr_t reg = x->op + OP_PORTSC + 0x10u * (port - 1);
  uint32_t v = rd32(reg);
  uint64_t start;

  if (!(v & PORT_CCS)) {
    return 0;
  }
  if (!(v & PORT_PP)) {
    wr32(reg, PORT_PP);
    delay_ms(100);
    v = rd32(reg);
  }
  wr32(reg, (v & PORT_PP) | PORT_CHANGE_BITS);            // clear the change bits
  if (!(rd32(reg) & PORT_PED)) {
    wr32(reg, PORT_PP | PORT_PR);
    start = timer_get_ms();
    while (!(rd32(reg) & PORT_PRC)) {
      if (timer_get_ms() - start > 1000) {
        return 0;
      }
    }
    wr32(reg, PORT_PP | PORT_PRC);
    delay_ms(20);
  }
  v = rd32(reg);
  if (!(v & PORT_CCS) || !(v & PORT_PED)) {
    return 0;
  }
  *speed = (int)((v >> 10) & 0xFu);
  return 1;
}

static xdev_t *address_device(xhci_t *x, uint32_t port, int speed, uint32_t route, int depth, int tt_slot,
                              int tt_port) {
  uint32_t slot = 0, cc;
  uint32_t *c;
  xdev_t *d;

  cc = command(x, 0, TRB_CMD_ENABLE << 10, &slot);
  if (cc != CC_SUCCESS || slot == 0 || slot > XHCI_MAX_SLOTS) {
    return 0;
  }
  d = &x->dev[slot];
  memset(d, 0, sizeof(*d));
  d->used = 1;
  d->slot = (int)slot;
  d->speed = speed;
  d->port = (int)port;
  d->route = route;
  d->depth = depth;
  d->tt_slot = tt_slot;
  d->tt_port = tt_port;
  d->ctx_phys = alloc_mapped(1);
  if (!d->ctx_phys || !ring_init(&d->ep0)) {
    return 0;
  }
  d->ctx = (uint8_t *)(uintptr_t)d->ctx_phys;
  x->dcbaa[slot] = d->ctx_phys;

  input_clear(x);
  c = ictx_ptr(x, 0);
  c[1] = 3;                                               // add slot + EP0
  fill_slot(d, ictx_ptr(x, 1), 1);
  ep_context(x, 1, 4, (uint32_t)speed_mps0(speed), &d->ep0, 8);
  cc = command(x, x->input_phys, (TRB_CMD_ADDRESS << 10) | (slot << 24), 0);
  return cc == CC_SUCCESS ? d : 0;
}

// Full-speed devices may have an 8-byte EP0; learn the real size and tell the controller.
static int fix_ep0_mps(xhci_t *x, xdev_t *d) {
  uint32_t mps, *c, *e;

  if (!control(x, d, 0x80, 6, 0x0100, 0, 8, 1)) {          // GET_DESCRIPTOR(device), 8 bytes
    return 0;
  }
  mps = x->dbuf[7];
  d->dev_class = x->dbuf[4];
  if (d->speed == 4) {
    mps = 512;
  }
  if (mps == 0 || mps == (uint32_t)speed_mps0(d->speed)) {
    return 1;
  }
  input_clear(x);
  c = ictx_ptr(x, 0);
  c[1] = 1u << 1;                                         // EP0 only
  e = ictx_ptr(x, 2);
  e[1] = (3u << 1) | (4u << 3) | (mps << 16);
  return command(x, x->input_phys, (TRB_CMD_EVALUATE << 10) | ((uint32_t)d->slot << 24), 0) == CC_SUCCESS;
}

// Look through the configuration for a SCSI bulk-only mass-storage interface and for
// boot-protocol keyboards and mice. Fills d->has_storage, d->hid[] and d->hid_count.
static void parse_config(xdev_t *d, const uint8_t *p, uint32_t len) {
  uint32_t i = 0;
  int in_storage = 0, hid = -1;

  d->cfg_value = p[5];
  while (i + 2 <= len && p[i] != 0) {
    uint8_t dlen = p[i], type = p[i + 1];

    if (i + dlen > len) {
      break;
    }
    if (type == 4 && dlen >= 9) {                         // interface
      uint8_t cls = p[i + 5], sub = p[i + 6], proto = p[i + 7];

      in_storage = (cls == 8 && sub == 6 && proto == 0x50 && p[i + 3] == 0 && g_disk_count < USB_MAX_DISKS);
      if (in_storage) {
        d->iface = p[i + 2];
      }
      hid = -1;
      if (cls == 3 && sub == 1 && (proto == 1 || proto == 2) && p[i + 3] == 0 && d->hid_count < 2) {
        hid = d->hid_count++;
        d->hid[hid].kind = proto;
        d->hid[hid].iface = p[i + 2];
        d->hid[hid].ep = 0;
      }
    } else if (type == 0x21 && dlen >= 9 && hid >= 0) {   // HID descriptor: first class descriptor is the report one
      if (p[i + 5] >= 1 && p[i + 6] == 0x22) {
        d->hid[hid].desc_len = (uint16_t)(p[i + 7] | (p[i + 8] << 8));
      }
    } else if (type == 5 && dlen >= 7) {                  // endpoint
      uint16_t mps = (uint16_t)(p[i + 4] | (p[i + 5] << 8));
      uint8_t attr = p[i + 3] & 3u;

      if (in_storage && attr == 2) {
        if (p[i + 2] & 0x80) {
          d->in_ep = p[i + 2];
          d->in_mps = mps;
        } else {
          d->out_ep = p[i + 2];
          d->out_mps = mps;
        }
      } else if (hid >= 0 && attr == 3 && (p[i + 2] & 0x80) && d->hid[hid].ep == 0) {
        d->hid[hid].ep = p[i + 2];
        d->hid[hid].mps = mps & 0x7FFu;
        d->hid[hid].interval = p[i + 6];
      }
    }
    i += dlen;
  }
  d->has_storage = d->in_ep != 0 && d->out_ep != 0;
  {
    int k, n = 0;                                          // drop HID interfaces without an endpoint
    for (k = 0; k < d->hid_count; k++) {
      if (d->hid[k].ep != 0) {
        d->hid[n++] = d->hid[k];
      }
    }
    d->hid_count = n;
  }
}

static uint32_t hid_interval(int speed, uint8_t b) {
  uint32_t v;

  if (speed >= 3) {                                        // high / super speed: 2^(v) * 125 us
    v = b ? (uint32_t)b - 1u : 0u;
  } else {                                                 // low / full speed: b ms
    uint32_t ms = b ? b : 1u;
    v = 3;
    while ((1u << (v - 3)) < ms && v < 10) {
      v++;
    }
  }
  return v > 15 ? 15 : v;
}

// One Configure Endpoint command for everything the device offers that we use.
static int configure_endpoints(xhci_t *x, xdev_t *d) {
  uint32_t add = 1, max_dci = 1, *c;
  int k;

  input_clear(x);
  if (d->has_storage) {
    uint32_t in_dci = (uint32_t)(d->in_ep & 0xFu) * 2 + 1;
    uint32_t out_dci = (uint32_t)(d->out_ep & 0xFu) * 2;

    if (!ring_init(&d->bulk_in) || !ring_init(&d->bulk_out)) {
      return 0;
    }
    add |= (1u << in_dci) | (1u << out_dci);
    max_dci = in_dci > out_dci ? in_dci : out_dci;
    ep_context(x, in_dci, 6, d->in_mps, &d->bulk_in, d->in_mps);
    ep_context(x, out_dci, 2, d->out_mps, &d->bulk_out, d->out_mps);
  }
  for (k = 0; k < d->hid_count; k++) {
    hid_t *h = &d->hid[k];
    uint32_t dci = (uint32_t)(h->ep & 0xFu) * 2 + 1;
    uint64_t buf = alloc_mapped(1);

    if (!ring_init(&h->ring) || !buf) {
      return 0;
    }
    h->dci = dci;
    h->buf_phys = buf;
    h->buf = (uint8_t *)(uintptr_t)buf;
    h->reqlen = h->mps < 8 ? h->mps : 8;
    if (h->reqlen == 0) {
      h->reqlen = 8;
    }
    add |= 1u << dci;
    if (dci > max_dci) {
      max_dci = dci;
    }
    ep_context(x, dci, 7, h->mps, &h->ring, h->mps);
    ictx_ptr(x, dci + 1)[0] = hid_interval(d->speed, h->interval) << 16;
  }
  c = ictx_ptr(x, 0);
  c[1] = add;
  fill_slot(d, ictx_ptr(x, 1), max_dci);
  return command(x, x->input_phys, (TRB_CMD_CONFIGURE << 10) | ((uint32_t)d->slot << 24), 0) == CC_SUCCESS;
}

// ---- Bulk-Only Transport -----------------------------------------------------

static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// One bulk transfer of `len` bytes between the bounce window (+ offset) and the device.
// Returns the completion code (0 = timeout).
static uint32_t bulk(xhci_t *x, xdev_t *d, int in, uint64_t phys, uint32_t len) {
  ring_t *r = in ? &d->bulk_in : &d->bulk_out;
  uint32_t dci = in ? (uint32_t)(d->in_ep & 0xFu) * 2 + 1 : (uint32_t)(d->out_ep & 0xFu) * 2;
  uint32_t ev[4];

  ring_push(r, phys, len, (TRB_NORMAL << 10) | (1u << 5));
  ring_doorbell(x, (uint32_t)d->slot, dci);
  if (!event_wait(x, TRB_EV_TRANSFER, (uint32_t)d->slot, dci, ev)) {
    return 0;
  }
  return ev[2] >> 24;
}

// Clear a halted bulk endpoint: reset it in the controller, move its ring past the
// failed TRB and tell the device (CLEAR_FEATURE ENDPOINT_HALT).
static void recover_endpoint(xhci_t *x, xdev_t *d, int in) {
  ring_t *r = in ? &d->bulk_in : &d->bulk_out;
  uint32_t ep = in ? d->in_ep : d->out_ep;
  uint32_t dci = in ? (uint32_t)(d->in_ep & 0xFu) * 2 + 1 : (uint32_t)(d->out_ep & 0xFu) * 2;

  command(x, 0, (TRB_CMD_RESET_EP << 10) | (dci << 16) | ((uint32_t)d->slot << 24), 0);
  command(x, (r->phys + (uint64_t)r->enq * sizeof(trb_t)) | r->cycle,
          (TRB_CMD_SET_DEQ << 10) | (dci << 16) | ((uint32_t)d->slot << 24), 0);
  control(x, d, 0x02, 1, 0, (uint16_t)ep, 0, 0);
}

// Send one SCSI command; data (len bytes) goes through the bounce buffer. Returns 1 on success.
static int scsi(xhci_t *x, xdev_t *d, const uint8_t *cdb, uint32_t cdb_len, int in, uint32_t len) {
  uint8_t *cbw = x->dbuf + 2048;                           // 31 bytes
  uint8_t *csw = x->dbuf + 3072;                           // 13 bytes
  uint64_t cbw_phys = x->dbuf_phys + 2048, csw_phys = x->dbuf_phys + 3072;
  uint32_t tag = ++d->tag, cc, i;

  memset(cbw, 0, 31);
  cbw[0] = 'U'; cbw[1] = 'S'; cbw[2] = 'B'; cbw[3] = 'C';
  cbw[4] = (uint8_t)tag; cbw[5] = (uint8_t)(tag >> 8); cbw[6] = (uint8_t)(tag >> 16); cbw[7] = (uint8_t)(tag >> 24);
  cbw[8] = (uint8_t)len; cbw[9] = (uint8_t)(len >> 8); cbw[10] = (uint8_t)(len >> 16); cbw[11] = (uint8_t)(len >> 24);
  cbw[12] = in ? 0x80 : 0x00;
  cbw[14] = (uint8_t)cdb_len;
  for (i = 0; i < cdb_len && i < 16; i++) {
    cbw[15 + i] = cdb[i];
  }
  cc = bulk(x, d, 0, cbw_phys, 31);
  if (cc != CC_SUCCESS && cc != CC_SHORT) {
    if (cc == CC_STALL) {
      recover_endpoint(x, d, 0);
    }
    return 0;
  }
  if (len != 0) {
    cc = bulk(x, d, in, x->bounce_phys, len);
    if (cc == CC_STALL) {
      recover_endpoint(x, d, in);
    } else if (cc != CC_SUCCESS && cc != CC_SHORT) {
      return 0;
    }
  }
  cc = bulk(x, d, 1, csw_phys, 13);
  if (cc == CC_STALL) {
    recover_endpoint(x, d, 1);
    cc = bulk(x, d, 1, csw_phys, 13);
  }
  if (cc != CC_SUCCESS && cc != CC_SHORT) {
    return 0;
  }
  return csw[0] == 'U' && csw[1] == 'S' && csw[2] == 'B' && csw[3] == 'S' && csw[12] == 0;
}

// (The CSW tag is not compared: only one command is ever in flight.)

static void trim_text(char *s) {
  int n = (int)str_len(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == 0)) {
    s[--n] = 0;
  }
}

static int storage_probe(xhci_t *x, xdev_t *d, usb_disk_t *out) {
  uint8_t cdb[16];
  uint64_t start;
  int ready = 0, i;
  uint32_t last_lba, block;

  // INQUIRY
  memset(cdb, 0, sizeof(cdb));
  cdb[0] = 0x12;
  cdb[4] = 36;
  memset(x->bounce, 0, 64);
  if (!scsi(x, d, cdb, 6, 1, 36)) {
    return 0;
  }
  for (i = 0; i < 24; i++) {
    char ch = (char)x->bounce[8 + i];
    out->model[i] = (ch >= 32 && ch < 127) ? ch : ' ';
  }
  out->model[24] = 0;
  trim_text(out->model);

  // TEST UNIT READY until the medium answers (sticks take a moment after reset).
  start = timer_get_ms();
  while (timer_get_ms() - start < 5000) {
    memset(cdb, 0, sizeof(cdb));
    if (scsi(x, d, cdb, 6, 0, 0)) {
      ready = 1;
      break;
    }
    delay_ms(100);
  }
  if (!ready) {
    return 0;
  }
  memset(cdb, 0, sizeof(cdb));
  cdb[0] = 0x25;                                           // READ CAPACITY(10)
  if (!scsi(x, d, cdb, 10, 1, 8)) {
    return 0;
  }
  last_lba = be32(x->bounce);
  block = be32(x->bounce + 4);
  if (block != 512 || last_lba == 0xFFFFFFFFu) {
    return 0;                                              // other block sizes / huge disks: not yet
  }
  out->sectors = (uint64_t)last_lba + 1;
  return 1;
}

static void hid_arm(xhci_t *x, xdev_t *d, hid_t *h) {
  ring_push(&h->ring, h->buf_phys, h->reqlen, (TRB_NORMAL << 10) | (1u << 5) | (1u << 2));   // IOC | ISP
  ring_doorbell(x, (uint32_t)d->slot, h->dci);
}

static int parse_mouse_desc(const uint8_t *p, uint32_t len, hid_t *h);

static void enumerate_device(xhci_t *x, int xi, uint32_t port, int speed, uint32_t route, int depth, int tt_slot,
                             int tt_port);

// Hub class requests (USB 2.0 11.24, USB 3.0 10.14), sent on the hub's endpoint 0.
#define HUB_GET_STATUS        0
#define HUB_CLEAR_FEATURE     1
#define HUB_SET_FEATURE       3
#define HUB_PORT_RESET        4
#define HUB_PORT_POWER        8
#define HUB_C_PORT_CONNECTION 16

// wPortStatus in the low 16 bits, wPortChange in the high 16 bits.
static int hub_port_status(xhci_t *x, xdev_t *h, uint32_t port, uint32_t *st) {
  if (!control(x, h, 0xA3, HUB_GET_STATUS, 0, (uint16_t)port, 4, 1)) {
    return 0;
  }
  *st = (uint32_t)x->dbuf[0] | ((uint32_t)x->dbuf[1] << 8) | ((uint32_t)x->dbuf[2] << 16) |
        ((uint32_t)x->dbuf[3] << 24);
  return 1;
}

static void hub_port_feature(xhci_t *x, xdev_t *h, uint32_t port, int set, uint16_t feature) {
  control(x, h, 0x23, set ? HUB_SET_FEATURE : HUB_CLEAR_FEATURE, feature, (uint16_t)port, 0, 0);
}

static void hub_scan_port(xhci_t *x, int xi, xdev_t *h, uint32_t port) {
  uint32_t st = 0, route;
  uint64_t start;
  int speed, tt_slot = 0, tt_port = 0;

  if (!hub_port_status(x, h, port, &st) || !(st & 1u)) {            // nothing connected
    return;
  }
  if (h->depth >= 5) {                                               // does not fit in a route string
    console_printf("[usb] hub port %u: too many hubs deep, ignored\r\n", port);
    return;
  }
  hub_port_feature(x, h, port, 0, HUB_C_PORT_CONNECTION);
  hub_port_feature(x, h, port, 1, HUB_PORT_RESET);
  start = timer_get_ms();
  for (;;) {
    delay_ms(10);
    if (hub_port_status(x, h, port, &st) && (st & (1u << 20))) {     // C_PORT_RESET
      break;
    }
    if (timer_get_ms() - start > 1000) {
      console_printf("[usb] hub port %u: reset timed out\r\n", port);
      return;
    }
  }
  hub_port_feature(x, h, port, 0, 20);                                // clear C_PORT_RESET
  if (!hub_port_status(x, h, port, &st) || !(st & 1u) || !(st & 2u)) {   // still connected and enabled?
    return;
  }
  delay_ms(20);                                                       // reset recovery
  if (h->speed == 4) {
    speed = 4;
  } else if (st & (1u << 9)) {
    speed = 2;
  } else if (st & (1u << 10)) {
    speed = 3;
  } else {
    speed = 1;
  }
  if (speed < 3) {                                                    // behind a transaction translator
    if (h->speed == 3) {
      tt_slot = h->slot;
      tt_port = (int)port;
    } else {
      tt_slot = h->tt_slot;
      tt_port = h->tt_port;
    }
  }
  route = h->route | (port << (4 * h->depth));
  enumerate_device(x, xi, (uint32_t)h->port, speed, route, h->depth + 1, tt_slot, tt_port);
}

// Read the hub descriptor, tell the controller this is a hub, power the ports and enumerate
// whatever is plugged into them. Hot plug is not followed: the ports are looked at once.
static void hub_start(xhci_t *x, int xi, xdev_t *h) {
  uint32_t n, p;
  int ss = h->speed == 4;

  if (ss && !control(x, h, 0x20, 12, (uint16_t)h->depth, 0, 0, 0)) {   // SET_HUB_DEPTH
    console_printf("[usb] hub: SET_HUB_DEPTH failed\r\n");
    return;
  }
  if (!control(x, h, 0xA0, 6, ss ? 0x2A00 : 0x2900, 0, ss ? 12 : 9, 1) || x->dbuf[2] == 0) {
    console_printf("[usb] hub: no hub descriptor\r\n");
    return;
  }
  n = x->dbuf[2];
  if (n > 15) {                                                        // a route string nibble holds 1..15
    n = 15;
  }
  h->hub_ports = (int)n;
  h->hub_pwr_ms = (uint32_t)x->dbuf[5] * 2u;
  h->hub_ttt = ss ? 0u : (((uint32_t)x->dbuf[3] >> 5) & 3u);
  h->is_hub = 1;
  if (!configure_endpoints(x, h)) {                       // slot context: hub flag, port count, think time
    console_printf("[usb] hub: could not configure the hub\r\n");
    return;
  }
  console_printf("[usb] %s hub with %u ports (root port %d, tier %d)\r\n", ss ? "SuperSpeed" : "USB 2.0", n, h->port,
                 h->depth + 1);
  for (p = 1; p <= n; p++) {
    hub_port_feature(x, h, p, 1, HUB_PORT_POWER);
  }
  delay_ms(h->hub_pwr_ms + 100);
  for (p = 1; p <= n; p++) {
    hub_scan_port(x, xi, h, p);
  }
}

static void enumerate_device(xhci_t *x, int xi, uint32_t port, int speed, uint32_t route, int depth, int tt_slot,
                             int tt_port) {
  int k;
  xdev_t *d;
  uint32_t total;
  usb_disk_t *disk;

  d = address_device(x, port, speed, route, depth, tt_slot, tt_port);
  if (d == 0 || !fix_ep0_mps(x, d)) {
    return;
  }
  // Configuration descriptor: header first for the total length, then all of it.
  if (!control(x, d, 0x80, 6, 0x0200, 0, 9, 1)) {
    return;
  }
  total = (uint32_t)x->dbuf[2] | ((uint32_t)x->dbuf[3] << 8);
  if (total > 1024) {
    total = 1024;
  }
  if (total < 9 || !control(x, d, 0x80, 6, 0x0200, 0, (uint16_t)total, 1)) {
    return;
  }
  parse_config(d, x->dbuf, total);
  if (d->dev_class == 9) {
    if (control(x, d, 0x00, 9, d->cfg_value, 0, 0, 0)) {                               // SET_CONFIGURATION
      hub_start(x, xi, d);
    }
    return;
  }
  if (!d->has_storage && d->hid_count == 0) {
    console_printf("[usb] port %u: unsupported device (speed %d)\r\n", port, speed);
    return;
  }
  if (!control(x, d, 0x00, 9, d->cfg_value, 0, 0, 0) || !configure_endpoints(x, d)) {     // SET_CONFIGURATION
    console_printf("[usb] port %u: could not configure the device\r\n", port);
    return;
  }
  for (k = 0; k < d->hid_count; k++) {
    hid_t *h = &d->hid[k];

    if (h->kind == 2 && h->desc_len > 0 && control(x, d, 0x81, 6, 0x2200, h->iface, h->desc_len < 1024 ? h->desc_len : 1024, 1) &&
        parse_mouse_desc(x->dbuf, h->desc_len < 1024 ? h->desc_len : 1024, h) &&
        control(x, d, 0x21, 0x0B, 1, h->iface, 0, 0)) {   // SET_PROTOCOL: report protocol, the mouse has a wheel
      h->report_mode = 1;
      h->reqlen = h->rep_len;
    } else {
      control(x, d, 0x21, 0x0B, 0, h->iface, 0, 0);        // SET_PROTOCOL: boot protocol
    }
    control(x, d, 0x21, 0x0A, 0, h->iface, 0, 0);          // SET_IDLE: only report changes
    h->active = 1;
    g_hid_active++;
    hid_arm(x, d, h);
    console_printf("[usb] port %u: %s\r\n", port, h->kind == 1 ? "keyboard" : "mouse");
  }
  if (d->has_storage) {
    disk = &g_disks[g_disk_count];
    memset(disk, 0, sizeof(*disk));
    if (!storage_probe(x, d, disk)) {
      console_printf("[usb] port %u: storage device did not answer (or is not 512-byte blocks)\r\n", port);
      return;
    }
    disk->controller = xi;
    disk->slot = d->slot;
    d->is_storage = 1;
    g_disk_count++;
  }
}

static void enumerate_port(xhci_t *x, int xi, uint32_t port) {
  int speed = 0;

  if (!reset_port(x, port, &speed)) {
    return;
  }
  enumerate_device(x, xi, port, speed, 0, 0, 0, 0);
}


// ---- Keyboards and mice (boot protocol) ----------------------------------------

#define HID_QUEUE 64
#define HID_REPEAT_DELAY_MS 500
#define HID_REPEAT_RATE_MS  33

static ps2_event_t g_hq[HID_QUEUE];
static unsigned int g_hq_head = 0, g_hq_tail = 0;
static uint8_t g_kbd_prev[6];
static uint8_t g_kbd_mods = 0;
static int g_caps = 0;
static uint8_t g_rep_key = 0;
static uint64_t g_rep_next = 0;
static uint8_t g_mouse_buttons = 0;

static void hq_push(const ps2_event_t *ev) {
  unsigned int next = (g_hq_head + 1) % HID_QUEUE;

  if (next != g_hq_tail) {                                 // full: drop
    g_hq[g_hq_head] = *ev;
    g_hq_head = next;
  }
}

static unsigned int hid_mods(void) {
  return ((g_kbd_mods & 0x22) ? PS2_MOD_SHIFT : 0u) | ((g_kbd_mods & 0x11) ? PS2_MOD_CTRL : 0u) |
         ((g_kbd_mods & 0x44) ? PS2_MOD_ALT : 0u);
}

static const char kHidPlain[17] = {'\n', 27, '\b', '\t', ' ', '-', '=', '[', ']', '\\', 0, ';', '\'', '`', ',', '.', '/'};
static const char kHidShift[17] = {'\n', 27, '\b', '\t', ' ', '_', '+', '{', '}', '|', 0, ':', '"', '~', '<', '>', '?'};
static const char kHidDigitShift[10] = {'!', '@', '#', '$', '%', '^', '&', '*', '(', ')'};

// ASCII (or a PS2_KEY_* code) for a HID usage, 0 if it has none.
static char hid_char(uint8_t u, int shift, int ctrl) {
  char ch = 0;

  if (u >= 0x04 && u <= 0x1D) {
    ch = (char)('a' + (u - 4));
    if (ctrl) {
      return (char)(ch - 'a' + 1);
    }
    if (shift ^ g_caps) {
      ch = (char)(ch - 'a' + 'A');
    }
    return ch;
  }
  if (u >= 0x1E && u <= 0x27) {
    return shift ? kHidDigitShift[u - 0x1E] : (char)(u == 0x27 ? '0' : '1' + (u - 0x1E));
  }
  if (u >= 0x28 && u <= 0x38) {
    return shift ? kHidShift[u - 0x28] : kHidPlain[u - 0x28];
  }
  switch (u) {
    case 0x4A: return PS2_KEY_HOME;
    case 0x4B: return PS2_KEY_PGUP;
    case 0x4C: return PS2_KEY_DELETE;
    case 0x4D: return PS2_KEY_END;
    case 0x4E: return PS2_KEY_PGDN;
    case 0x4F: return PS2_KEY_RIGHT;
    case 0x50: return PS2_KEY_LEFT;
    case 0x51: return PS2_KEY_DOWN;
    case 0x52: return PS2_KEY_UP;
    default: return 0;
  }
}

static void kbd_press(uint8_t key) {
  ps2_event_t ev;
  char ch;

  if (key == 0x39) {                                       // caps lock toggles on press
    g_caps = !g_caps;
    return;
  }
  ch = hid_char(key, (g_kbd_mods & 0x22) != 0, (g_kbd_mods & 0x11) != 0);
  if (ch == 0) {
    return;
  }
  memset(&ev, 0, sizeof(ev));
  ev.type = PS2_EVENT_KEY;
  ev.ch = ch;
  ev.pressed = 1;
  ev.mods = hid_mods();
  hq_push(&ev);
  g_rep_key = key;
  g_rep_next = timer_get_ms() + HID_REPEAT_DELAY_MS;
}

static void kbd_report(const uint8_t *r) {
  int i, j;

  g_kbd_mods = r[0];
  if (r[2] == 1) {                                         // phantom state (too many keys)
    return;
  }
  for (i = 0; i < 6; i++) {                                // keys that went up
    int still = 0;
    for (j = 0; j < 6; j++) {
      if (r[2 + j] == g_kbd_prev[i]) {
        still = 1;
      }
    }
    if (g_kbd_prev[i] != 0 && !still && g_kbd_prev[i] == g_rep_key) {
      g_rep_key = 0;
    }
  }
  for (i = 0; i < 6; i++) {                                // keys that went down
    int was = 0;
    for (j = 0; j < 6; j++) {
      if (r[2 + i] == g_kbd_prev[j]) {
        was = 1;
      }
    }
    if (r[2 + i] != 0 && !was) {
      kbd_press(r[2 + i]);
    }
  }
  for (i = 0; i < 6; i++) {
    g_kbd_prev[i] = r[2 + i];
  }
}

// Bits [off, off + size) of a report as a signed or unsigned number (size <= 16).
static int field_get(const uint8_t *b, int off, int size, int sign) {
  int v = 0, i;

  for (i = 0; i < size; i++) {
    int bit = off + i;

    v |= ((b[bit >> 3] >> (bit & 7)) & 1) << i;
  }
  if (sign && size < 32 && (v & (1 << (size - 1)))) {
    v -= 1 << size;
  }
  return v;
}

// Walk a HID report descriptor for a mouse report with X, Y and a wheel (Generic Desktop
// 0x30 / 0x31 / 0x38) and note where each field sits. Returns 1 if all three are there in
// one report that fits an interrupt transfer.
static int parse_mouse_desc(const uint8_t *p, uint32_t len, hid_t *h) {
  uint32_t i = 0, page = 0, rsize = 0, rcount = 0, rid = 0;
  uint32_t pos[16];
  uint32_t usages[16], umin = 0, umax = 0;
  int nus = 0, have_range = 0, lmin = 0;
  int xr = -1, yr = -1, wr = -1, br = -1;
  int bo = 0, xo = 0, yo = 0, wo = 0, xs = 0, ys = 0, ws = 0;
  uint32_t k;

  for (k = 0; k < 16; k++) {
    pos[k] = 0;
  }
  while (i < len) {
    uint8_t b = p[i];
    uint32_t sz = b & 3u, val = 0, type = (b >> 2) & 3u, tag = b >> 4;
    int sval;

    if (b == 0xFE) {                                       // long item: skip
      i += 3u + (i + 1 < len ? p[i + 1] : 0u);
      continue;
    }
    if (sz == 3) {
      sz = 4;
    }
    if (i + 1 + sz > len) {
      break;
    }
    for (k = 0; k < sz; k++) {
      val |= (uint32_t)p[i + 1 + k] << (8 * k);
    }
    sval = sz == 1 ? (int)(signed char)val : (sz == 2 ? (int)(short)val : (int)val);
    i += 1 + sz;
    if (type == 1) {                                       // global
      if (tag == 0) page = val;
      else if (tag == 1) lmin = sval;
      else if (tag == 7) rsize = val;
      else if (tag == 8) rid = val < 16 ? val : 15;
      else if (tag == 9) rcount = val;
    } else if (type == 2) {                                // local
      if (tag == 0 && nus < 16) usages[nus++] = val & 0xFFFFu;
      else if (tag == 1) umin = val & 0xFFFFu;
      else if (tag == 2) { umax = val & 0xFFFFu; have_range = 1; }
    } else if (type == 0) {                                // main
      if (tag == 8) {                                      // Input
        uint32_t idx;

        for (idx = 0; idx < rcount && idx < 256; idx++) {
          uint32_t u = 0;
          int off = (int)(pos[rid] + idx * rsize);

          if (!(val & 1u)) {                               // data, not constant
            if (nus > 0) {
              u = usages[idx < (uint32_t)nus ? idx : (uint32_t)nus - 1];
            } else if (have_range && umin + idx <= umax) {
              u = umin + idx;
            }
            if (page == 9 && u == 1 && br < 0) {
              br = (int)rid;
              bo = off;
            } else if (page == 1 && u == 0x30 && xr < 0) {
              xr = (int)rid; xo = off; xs = (int)rsize; (void)lmin;
            } else if (page == 1 && u == 0x31 && yr < 0) {
              yr = (int)rid; yo = off; ys = (int)rsize;
            } else if (page == 1 && u == 0x38 && wr < 0) {
              wr = (int)rid; wo = off; ws = (int)rsize;
            }
          }
        }
        pos[rid] += rcount * rsize;
      }
      nus = 0;
      have_range = 0;
    }
  }
  if (xr < 0 || yr != xr || wr != xr || br != xr || xs > 16 || ys > 16 || ws > 16 || xs == 0 || ys == 0 || ws == 0) {
    return 0;
  }
  h->has_id = xr != 0;
  h->rid = (uint8_t)xr;
  h->rep_len = (pos[xr] + 7u) / 8u + (h->has_id ? 1u : 0u);
  if (h->rep_len > 64) {
    return 0;
  }
  {
    int shift = h->has_id ? 8 : 0;

    h->btn_off = bo + shift;
    h->x_off = xo + shift;
    h->y_off = yo + shift;
    h->w_off = wo + shift;
  }
  h->x_sz = xs;
  h->y_sz = ys;
  h->w_sz = ws;
  return 1;
}

static void mouse_emit(int dx, int dy, uint8_t buttons, int wheel) {
  ps2_event_t ev;

  if (dx == 0 && dy == 0 && wheel == 0 && buttons == g_mouse_buttons) {
    return;
  }
  g_mouse_buttons = buttons;
  memset(&ev, 0, sizeof(ev));
  ev.type = PS2_EVENT_MOUSE;
  ev.dx = dx;
  ev.dy = dy;
  ev.wheel = wheel;
  ev.buttons = buttons;                                    // same bit order as PS2_MOUSE_*
  ev.mods = hid_mods();
  hq_push(&ev);
}

static void mouse_report(const uint8_t *r) {
  mouse_emit((signed char)r[1], (signed char)r[2], r[0] & 7u, 0);
}

// Report protocol: fields where the report descriptor put them. HID wheel: positive = forward.
static void mouse_report_desc(const hid_t *h) {
  const uint8_t *r = h->buf;

  if (h->has_id && r[0] != h->rid) {
    return;
  }
  mouse_emit(field_get(r, h->x_off, h->x_sz, 1), field_get(r, h->y_off, h->y_sz, 1),
             (uint8_t)field_get(r, h->btn_off, 3, 0), field_get(r, h->w_off, h->w_sz, 1));
}

// A transfer event: if it belongs to a keyboard or mouse endpoint, decode the report and
// queue the next one.
static void hid_event(xhci_t *x, const uint32_t ev[4]) {
  uint32_t slot = ev[3] >> 24, dci = (ev[3] >> 16) & 0x1Fu, cc = ev[2] >> 24;
  uint32_t left = ev[2] & 0xFFFFFFu;
  xdev_t *d;
  int k;

  if (slot == 0 || slot > XHCI_MAX_SLOTS) {
    return;
  }
  d = &x->dev[slot];
  for (k = 0; k < d->hid_count; k++) {
    hid_t *h = &d->hid[k];

    if (!h->active || h->dci != dci) {
      continue;
    }
    if (cc == CC_SUCCESS || cc == CC_SHORT) {
      uint32_t got = left <= h->reqlen ? h->reqlen - left : 0;

      if (h->kind == 1 && got >= 8) {
        kbd_report(h->buf);
      } else if (h->kind == 2 && h->report_mode && got >= h->rep_len) {
        mouse_report_desc(h);
      } else if (h->kind == 2 && !h->report_mode && got >= 3) {
        mouse_report(h->buf);
      }
      hid_arm(x, d, h);
    } else {
      h->active = 0;                                       // stalled or failed: stop polling it
      g_hid_active--;
    }
    return;
  }
}

int usbhid_poll(ps2_event_t *ev) {
  int i;

  if (g_hid_active == 0) {
    return 0;
  }
  if (g_hq_head == g_hq_tail) {
    uint64_t now;

    scheduler_preempt_disable();                           // storage transfers share the event ring
    for (i = 0; i < g_x_count; i++) {
      uint32_t e[4];

      if (!g_x[i].ready) {
        continue;
      }
      while (event_next(&g_x[i], e)) {
        if (((e[3] >> 10) & 0x3Fu) == TRB_EV_TRANSFER) {
          hid_event(&g_x[i], e);
        }
      }
    }
    scheduler_preempt_enable();
    now = timer_get_ms();
    if (g_rep_key != 0 && now >= g_rep_next && g_hq_head == g_hq_tail) {     // typematic repeat
      char ch = hid_char(g_rep_key, (g_kbd_mods & 0x22) != 0, (g_kbd_mods & 0x11) != 0);

      if (ch != 0) {
        ps2_event_t r;

        memset(&r, 0, sizeof(r));
        r.type = PS2_EVENT_KEY;
        r.ch = ch;
        r.pressed = 1;
        r.mods = hid_mods();
        hq_push(&r);
      }
      g_rep_next = now + HID_REPEAT_RATE_MS;
    }
  }
  if (g_hq_head == g_hq_tail) {
    return 0;
  }
  *ev = g_hq[g_hq_tail];
  g_hq_tail = (g_hq_tail + 1) % HID_QUEUE;
  return 1;
}

static int usb_scan(void);

// Disks are brought up with interrupts off, but USB needs real time (port resets, command
// time-outs) and the clock only ticks from the timer interrupt. So interrupts are on while
// the bus is scanned, with task switching held off so nothing else runs meanwhile.
int usbstor_init(void) {
  uint64_t flags;
  int n;

  __asm__ volatile("pushfq; popq %0" : "=r"(flags) : : "memory");
  scheduler_preempt_disable();
  __asm__ volatile("sti" : : : "memory");
  n = usb_scan();
  if (!(flags & 0x200ULL)) {
    __asm__ volatile("cli" : : : "memory");
  }
  scheduler_preempt_enable();
  return n;
}

static int usb_scan(void) {
  pci_dev_t dev;
  int index;

  g_x_count = 0;
  g_disk_count = 0;
  for (index = 0; index < XHCI_MAX_CONTROLLERS; index++) {
    xhci_t *x = &g_x[g_x_count];
    uint32_t port;

    if (!pci_find_class(0x0C, 0x03, 0x30, index, &dev)) {
      break;
    }
    g_x_count++;
    if (!controller_start(x, &dev)) {
      console_printf("[usb] xHCI controller %d did not start\r\n", index);
      continue;
    }
    x->ready = 1;
    for (port = 1; port <= x->max_ports; port++) {
      if (rd32(x->op + OP_PORTSC + 0x10u * (port - 1)) & PORT_CCS) {
        enumerate_port(x, g_x_count - 1, port);
      }
    }
  }
  return g_disk_count;
}

int usbstor_disk_count(void) { return g_disk_count; }

const usb_disk_t *usbstor_disk(int index) {
  return (index >= 0 && index < g_disk_count) ? &g_disks[index] : 0;
}

static int transfer_locked(int index, uint64_t lba, uint32_t count, uint8_t *buf, int write) {
  const usb_disk_t *disk = usbstor_disk(index);
  xhci_t *x;
  xdev_t *d;

  if (!disk) {
    return 0;
  }
  x = &g_x[disk->controller];
  d = &x->dev[disk->slot];
  if (!x->ready || !d->is_storage || lba + count > disk->sectors || lba > 0xFFFFFFFFu) {
    return 0;
  }
  while (count > 0) {
    uint32_t chunk = count > 128 ? 128 : count;             // 64 KiB per command
    uint32_t bytes = chunk * 512u;
    uint8_t cdb[16];

    if (write) {
      memcpy(x->bounce, buf, bytes);
    }
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = write ? 0x2A : 0x28;                           // WRITE(10) / READ(10)
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)lba;
    cdb[7] = (uint8_t)(chunk >> 8);
    cdb[8] = (uint8_t)chunk;
    if (!scsi(x, d, cdb, 10, !write, bytes)) {
      return 0;
    }
    if (!write) {
      memcpy(buf, x->bounce, bytes);
    }
    lba += chunk;
    buf += bytes;
    count -= chunk;
  }
  return 1;
}

// The event ring is shared with the keyboard and mouse polling, so a storage transfer
// runs with preemption off (like the FAT driver does).
static int transfer(int index, uint64_t lba, uint32_t count, uint8_t *buf, int write) {
  int ok;

  scheduler_preempt_disable();
  ok = transfer_locked(index, lba, count, buf, write);
  scheduler_preempt_enable();
  return ok;
}

int usbstor_read(int index, uint64_t lba, uint32_t count, void *buf) {
  return transfer(index, lba, count, (uint8_t *)buf, 0);
}

int usbstor_write(int index, uint64_t lba, uint32_t count, const void *buf) {
  return transfer(index, lba, count, (uint8_t *)(uintptr_t)buf, 1);
}
