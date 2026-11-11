#include "usbstor.h"
#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "string.h"
#include "timer.h"

// xHCI host controller + USB mass storage (Bulk-Only Transport, SCSI transparent
// command set). Everything is polled: one command or transfer in flight, one event
// ring, memory from the PMM (identity mapped, so physical == virtual). Devices must be
// plugged into the controller's own ports (no hubs). Only 512-byte-block devices
// are exposed.

#define XHCI_MAX_CONTROLLERS 2
#define XHCI_MAX_SLOTS       8
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
  int used;
  int slot;
  int speed;
  int port;
  uint8_t *ctx;              // output device context
  uint64_t ctx_phys;
  ring_t ep0;
  ring_t bulk_in, bulk_out;
  uint8_t in_ep, out_ep;     // endpoint addresses (bit 7 = IN)
  uint16_t in_mps, out_mps;
  uint8_t cfg_value, iface;
  uint32_t tag;
  int is_storage;
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

// Wait for an event of `type` (for transfer events also matching slot and endpoint id,
// ep_id 0 = any). Other events are discarded. Returns 1 and fills out[4].
static int event_wait(xhci_t *x, uint32_t type, uint32_t slot, uint32_t ep_id, uint32_t out[4]) {
  uint64_t start = timer_get_ms();

  for (;;) {
    trb_t *t = &x->ev[x->ev_deq];
    uint32_t c = t->d[3];

    if ((c & 1u) == x->ev_cycle) {
      uint32_t d0 = t->d[0], d1 = t->d[1], d2 = t->d[2];
      uint32_t et = (c >> 10) & 0x3Fu;
      uint64_t erdp;

      x->ev_deq++;
      if (x->ev_deq == EVENT_TRBS) {
        x->ev_deq = 0;
        x->ev_cycle ^= 1u;
      }
      erdp = x->ev_phys + (uint64_t)x->ev_deq * sizeof(trb_t);
      wr64(x->rt + 0x20 + 0x18, erdp | (1u << 3));     // interrupter 0 ERDP, clear EHB
      if (et == type && (type == TRB_EV_COMMAND ||
                         ((c >> 24) == slot && (ep_id == 0 || ((c >> 16) & 0x1Fu) == ep_id)))) {
        out[0] = d0; out[1] = d1; out[2] = d2; out[3] = c;
        return 1;
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

static xdev_t *address_device(xhci_t *x, uint32_t port, int speed) {
  uint32_t slot = 0, cc;
  uint32_t *c, *s;
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
  d->ctx_phys = alloc_mapped(1);
  if (!d->ctx_phys || !ring_init(&d->ep0)) {
    return 0;
  }
  d->ctx = (uint8_t *)(uintptr_t)d->ctx_phys;
  x->dcbaa[slot] = d->ctx_phys;

  input_clear(x);
  c = ictx_ptr(x, 0);
  c[1] = 3;                                               // add slot + EP0
  s = ictx_ptr(x, 1);
  s[0] = ((uint32_t)speed << 20) | (1u << 27);
  s[1] = port << 16;
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

// Look through the configuration for a SCSI bulk-only mass-storage interface.
static int parse_config(xdev_t *d, const uint8_t *p, uint32_t len) {
  uint32_t i = 0;
  int in_iface = 0;

  d->cfg_value = p[5];
  while (i + 2 <= len && p[i] != 0) {
    uint8_t dlen = p[i], type = p[i + 1];

    if (i + dlen > len) {
      break;
    }
    if (type == 4 && dlen >= 9) {                         // interface
      in_iface = (p[i + 5] == 8 && p[i + 6] == 6 && p[i + 7] == 0x50 && p[i + 3] == 0);
      if (in_iface) {
        d->iface = p[i + 2];
      }
    } else if (type == 5 && dlen >= 7 && in_iface && (p[i + 3] & 3u) == 2) {     // bulk endpoint
      uint16_t mps = (uint16_t)(p[i + 4] | (p[i + 5] << 8));
      if (p[i + 2] & 0x80) {
        d->in_ep = p[i + 2];
        d->in_mps = mps;
      } else {
        d->out_ep = p[i + 2];
        d->out_mps = mps;
      }
    }
    i += dlen;
  }
  return d->in_ep != 0 && d->out_ep != 0;
}

static int configure_storage(xhci_t *x, xdev_t *d) {
  uint32_t in_dci = (uint32_t)(d->in_ep & 0xFu) * 2 + 1;
  uint32_t out_dci = (uint32_t)(d->out_ep & 0xFu) * 2;
  uint32_t max_dci = in_dci > out_dci ? in_dci : out_dci;
  uint32_t *c, *s;

  if (!ring_init(&d->bulk_in) || !ring_init(&d->bulk_out)) {
    return 0;
  }
  input_clear(x);
  c = ictx_ptr(x, 0);
  c[1] = 1u | (1u << in_dci) | (1u << out_dci);
  s = ictx_ptr(x, 1);
  s[0] = ((uint32_t)d->speed << 20) | (max_dci << 27);
  s[1] = (uint32_t)d->port << 16;
  ep_context(x, in_dci, 6, d->in_mps, &d->bulk_in, d->in_mps);
  ep_context(x, out_dci, 2, d->out_mps, &d->bulk_out, d->out_mps);
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

static void enumerate_port(xhci_t *x, int xi, uint32_t port) {
  int speed = 0;
  xdev_t *d;
  uint32_t total;
  usb_disk_t *disk;

  if (g_disk_count >= USB_MAX_DISKS || !reset_port(x, port, &speed)) {
    return;
  }
  d = address_device(x, port, speed);
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
  if (!parse_config(d, x->dbuf, total)) {
    console_printf("[usb] port %u: not a mass-storage device (speed %d)\r\n", port, speed);
    return;
  }
  if (!control(x, d, 0x00, 9, d->cfg_value, 0, 0, 0) || !configure_storage(x, d)) {     // SET_CONFIGURATION
    console_printf("[usb] port %u: could not configure the device\r\n", port);
    return;
  }
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

int usbstor_init(void) {
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

static int transfer(int index, uint64_t lba, uint32_t count, uint8_t *buf, int write) {
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

int usbstor_read(int index, uint64_t lba, uint32_t count, void *buf) {
  return transfer(index, lba, count, (uint8_t *)buf, 0);
}

int usbstor_write(int index, uint64_t lba, uint32_t count, const void *buf) {
  return transfer(index, lba, count, (uint8_t *)(uintptr_t)buf, 1);
}
