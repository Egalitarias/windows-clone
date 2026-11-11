// Structured exception handling for the Win32 runtime: the x64 unwinder (RtlLookupFunctionEntry and
// RtlVirtualUnwind read the program's .pdata and unwind info), a two-phase dispatcher, and
// __C_specific_handler for __try/__except/__finally. The C++ part (catch blocks, destructors) is
// cxxeh.c, which plugs into the same dispatcher.
//
// Raising an exception captures the registers and walks the stack. Phase 1 asks each frame's
// language handler whether it will handle the exception (a handler that will, records itself in
// the dispatch's `found`); phase 2 walks again, this time calling the handlers to run __finally
// blocks and destructors, and stops at the frame that found the handler, where execution resumes
// (an __except block, or the continuation address a C++ catch block returns).
//
// Not supported: faults (a page fault, a division by zero) do not reach this code, they still end
// the process; a filter that answers EXCEPTION_CONTINUE_EXECUTION is treated as "keep looking";
// exceptions thrown while an exception is being unwound by a destructor.
#include "eh.h"

// ---- modules and function tables ----------------------------------------------------------------

static int module_info(int i, ULONG64 *base, ULONG *size) {
  ULONG64 b = i == 0 ? rt_exe_base() : rt_dll_base();
  const uint8_t *nt;

  if (b == 0) {
    return 0;
  }
  nt = (const uint8_t *)(uintptr_t)b + *(const uint32_t *)(uintptr_t)(b + 0x3C);
  *base = b;
  *size = *(const uint32_t *)(nt + 24 + 56);
  return 1;
}

int rt_eh_find_module(ULONG64 addr, ULONG64 *base) {
  int i;

  for (i = 0; i < 2; ++i) {
    ULONG64 b;
    ULONG size;

    if (module_info(i, &b, &size) && addr >= b && addr < b + size) {
      if (base != 0) {
        *base = b;
      }
      return 1;
    }
  }
  return 0;
}

EXPORT PRUNTIME_FUNCTION WINAPI RtlLookupFunctionEntry(DWORD64 pc, PDWORD64 image_base, PUNWIND_HISTORY_TABLE history) {
  ULONG64 base;
  const uint8_t *nt;
  ULONG rva, size, lo, hi;
  const RUNTIME_FUNCTION *table;

  (void)history;
  if (!rt_eh_find_module(pc, &base)) {
    if (image_base != 0) {
      *image_base = 0;
    }
    return 0;
  }
  if (image_base != 0) {
    *image_base = base;
  }
  nt = (const uint8_t *)(uintptr_t)base + *(const uint32_t *)(uintptr_t)(base + 0x3C);
  rva = *(const uint32_t *)(nt + 24 + 112 + 3 * 8);
  size = *(const uint32_t *)(nt + 24 + 112 + 3 * 8 + 4);
  if (rva == 0 || size == 0) {
    return 0;
  }
  table = (const RUNTIME_FUNCTION *)(uintptr_t)(base + rva);
  lo = 0;
  hi = size / (ULONG)sizeof(RUNTIME_FUNCTION);
  while (lo < hi) {
    ULONG mid = lo + (hi - lo) / 2;
    ULONG64 begin = table[mid].BeginAddress, end = table[mid].EndAddress, off = pc - base;

    if (off < begin) {
      hi = mid;
    } else if (off >= end) {
      lo = mid + 1;
    } else {
      const RUNTIME_FUNCTION *rf = &table[mid];

      while (rf->UnwindData & 1) {                    // the unwind data is another function entry
        rf = (const RUNTIME_FUNCTION *)(uintptr_t)(base + (rf->UnwindData & ~1u));
      }
      return (PRUNTIME_FUNCTION)rf;
    }
  }
  return 0;
}

// Undo the effect of the function's prolog on `ctx` (making it the caller's context) and find the
// function's exception handler, if it has one of the wanted `type` (UNW_FLAG_EHANDLER/UHANDLER).
EXPORT PEXCEPTION_ROUTINE WINAPI RtlVirtualUnwind(DWORD type, DWORD64 base, DWORD64 pc, PRUNTIME_FUNCTION rf, PCONTEXT ctx,
                                                  PVOID *handler_data, PDWORD64 establisher, PKNONVOLATILE_CONTEXT_POINTERS ptrs) {
  PEXCEPTION_ROUTINE handler = 0;
  DWORD64 offset = pc - (base + rf->BeginAddress);
  int first = 1;
  DWORD64 *regs = &ctx->Rax;

  (void)ptrs;
  if (handler_data != 0) {
    *handler_data = 0;
  }
  for (;;) {
    const UNWIND_INFO *ui = (const UNWIND_INFO *)(uintptr_t)(base + rf->UnwindData);
    ULONG flags = ui->Flags, count = ui->CountOfCodes, slots = (count + 1u) & ~1u, i;

    if (first) {
      DWORD64 frame = ui->FrameRegister != 0 ? regs[ui->FrameRegister] - (DWORD64)ui->FrameOffset * 16 : ctx->Rsp;

      if (establisher != 0) {
        *establisher = frame;
      }
    }
    for (i = 0; i < count;) {
      const UNWIND_CODE *c = &ui->UnwindCode[i];
      int apply = !first || offset >= c->CodeOffset;
      ULONG n = 1;

      switch (c->UnwindOp) {
        case UWOP_PUSH_NONVOL:
          if (apply) {
            regs[c->OpInfo] = *(DWORD64 *)(uintptr_t)ctx->Rsp;
            ctx->Rsp += 8;
          }
          break;
        case UWOP_ALLOC_LARGE: {
          DWORD64 size;

          if (c->OpInfo == 0) {
            size = (DWORD64)ui->UnwindCode[i + 1].FrameOffset * 8;
            n = 2;
          } else {
            size = (DWORD64)ui->UnwindCode[i + 1].FrameOffset | ((DWORD64)ui->UnwindCode[i + 2].FrameOffset << 16);
            n = 3;
          }
          if (apply) {
            ctx->Rsp += size;
          }
          break;
        }
        case UWOP_ALLOC_SMALL:
          if (apply) {
            ctx->Rsp += (DWORD64)c->OpInfo * 8 + 8;
          }
          break;
        case UWOP_SET_FPREG:
          if (apply) {
            ctx->Rsp = regs[ui->FrameRegister] - (DWORD64)ui->FrameOffset * 16;
          }
          break;
        case UWOP_SAVE_NONVOL:
          if (apply) {
            regs[c->OpInfo] = *(DWORD64 *)(uintptr_t)(ctx->Rsp + (DWORD64)ui->UnwindCode[i + 1].FrameOffset * 8);
          }
          n = 2;
          break;
        case UWOP_SAVE_NONVOL_FAR:
          if (apply) {
            DWORD64 off = (DWORD64)ui->UnwindCode[i + 1].FrameOffset | ((DWORD64)ui->UnwindCode[i + 2].FrameOffset << 16);

            regs[c->OpInfo] = *(DWORD64 *)(uintptr_t)(ctx->Rsp + off);
          }
          n = 3;
          break;
        case UWOP_SAVE_XMM128:
          if (apply) {
            (&ctx->Xmm0)[c->OpInfo] = *(M128A *)(uintptr_t)(ctx->Rsp + (DWORD64)ui->UnwindCode[i + 1].FrameOffset * 16);
          }
          n = 2;
          break;
        case UWOP_SAVE_XMM128_FAR:
          if (apply) {
            DWORD64 off = (DWORD64)ui->UnwindCode[i + 1].FrameOffset | ((DWORD64)ui->UnwindCode[i + 2].FrameOffset << 16);

            (&ctx->Xmm0)[c->OpInfo] = *(M128A *)(uintptr_t)(ctx->Rsp + off);
          }
          n = 3;
          break;
        case UWOP_PUSH_MACHFRAME:
          if (apply) {
            if (c->OpInfo != 0) {
              ctx->Rsp += 8;                          // the error code
            }
            ctx->Rip = *(DWORD64 *)(uintptr_t)ctx->Rsp;
            ctx->Rsp = *(DWORD64 *)(uintptr_t)(ctx->Rsp + 0x18);
            return handler;
          }
          break;
        default:
          break;                                      // UWOP_EPILOG and the like: no effect on the registers
      }
      i += n;
    }
    if ((flags & UNW_FLAG_CHAININFO) == 0) {
      if ((flags & type) != 0) {
        const uint8_t *p = (const uint8_t *)&ui->UnwindCode[slots];

        handler = (PEXCEPTION_ROUTINE)(uintptr_t)(base + *(const uint32_t *)p);
        if (handler_data != 0) {
          *handler_data = (PVOID)(p + 4);
        }
      }
      break;
    }
    rf = (PRUNTIME_FUNCTION)&ui->UnwindCode[slots];     // the parent entry of a chained function
    first = 0;
  }
  ctx->Rip = *(DWORD64 *)(uintptr_t)ctx->Rsp;
  ctx->Rsp += 8;
  return handler;
}

// ---- capturing and restoring a context ------------------------------------------------------------------------
// Offsets into CONTEXT: Rax 0x78 ... R15 0xF0, Rip 0xF8, Xmm0 0x1A0, MxCsr 0x34, EFlags 0x44.

_Static_assert(__builtin_offsetof(CONTEXT, Rax) == 0x78 && __builtin_offsetof(CONTEXT, Rsp) == 0x98 &&
               __builtin_offsetof(CONTEXT, R15) == 0xF0 && __builtin_offsetof(CONTEXT, Rip) == 0xF8 &&
               __builtin_offsetof(CONTEXT, Xmm0) == 0x1A0 && __builtin_offsetof(CONTEXT, EFlags) == 0x44 &&
               __builtin_offsetof(CONTEXT, MxCsr) == 0x34, "CONTEXT layout");

EXPORT __attribute__((naked)) VOID WINAPI RtlCaptureContext(PCONTEXT ctx) {
  __asm__(".intel_syntax noprefix\n\t"
      "mov [rcx+0x78], rax\n\t"
      "mov [rcx+0x80], rcx\n\t"
      "mov [rcx+0x88], rdx\n\t"
      "mov [rcx+0x90], rbx\n\t"
      "lea rax, [rsp+8]\n\t"
      "mov [rcx+0x98], rax\n\t"
      "mov [rcx+0xA0], rbp\n\t"
      "mov [rcx+0xA8], rsi\n\t"
      "mov [rcx+0xB0], rdi\n\t"
      "mov [rcx+0xB8], r8\n\t"
      "mov [rcx+0xC0], r9\n\t"
      "mov [rcx+0xC8], r10\n\t"
      "mov [rcx+0xD0], r11\n\t"
      "mov [rcx+0xD8], r12\n\t"
      "mov [rcx+0xE0], r13\n\t"
      "mov [rcx+0xE8], r14\n\t"
      "mov [rcx+0xF0], r15\n\t"
      "mov rax, [rsp]\n\t"
      "mov [rcx+0xF8], rax\n\t"
      "movups [rcx+0x1A0], xmm0\n\t"
      "movups [rcx+0x1B0], xmm1\n\t"
      "movups [rcx+0x1C0], xmm2\n\t"
      "movups [rcx+0x1D0], xmm3\n\t"
      "movups [rcx+0x1E0], xmm4\n\t"
      "movups [rcx+0x1F0], xmm5\n\t"
      "movups [rcx+0x200], xmm6\n\t"
      "movups [rcx+0x210], xmm7\n\t"
      "movups [rcx+0x220], xmm8\n\t"
      "movups [rcx+0x230], xmm9\n\t"
      "movups [rcx+0x240], xmm10\n\t"
      "movups [rcx+0x250], xmm11\n\t"
      "movups [rcx+0x260], xmm12\n\t"
      "movups [rcx+0x270], xmm13\n\t"
      "movups [rcx+0x280], xmm14\n\t"
      "movups [rcx+0x290], xmm15\n\t"
      "stmxcsr [rcx+0x34]\n\t"
      "pushfq\n\t"
      "pop rax\n\t"
      "mov [rcx+0x44], eax\n\t"
      "mov dword ptr [rcx+0x30], 0x10000B\n\t"
      "mov rax, [rcx+0x78]\n\t"
      "ret\n\t"
      ".att_syntax\n");
}

// Load every register from `ctx` (rcx) and continue at its Rip with its Rsp.
EXPORT __attribute__((naked)) VOID WINAPI RtlRestoreContext(PCONTEXT ctx, PEXCEPTION_RECORD rec) {
  __asm__(".intel_syntax noprefix\n\t"
      "mov r11, rcx\n\t"
      "movups xmm0, [r11+0x1A0]\n\t"
      "movups xmm1, [r11+0x1B0]\n\t"
      "movups xmm2, [r11+0x1C0]\n\t"
      "movups xmm3, [r11+0x1D0]\n\t"
      "movups xmm4, [r11+0x1E0]\n\t"
      "movups xmm5, [r11+0x1F0]\n\t"
      "movups xmm6, [r11+0x200]\n\t"
      "movups xmm7, [r11+0x210]\n\t"
      "movups xmm8, [r11+0x220]\n\t"
      "movups xmm9, [r11+0x230]\n\t"
      "movups xmm10, [r11+0x240]\n\t"
      "movups xmm11, [r11+0x250]\n\t"
      "movups xmm12, [r11+0x260]\n\t"
      "movups xmm13, [r11+0x270]\n\t"
      "movups xmm14, [r11+0x280]\n\t"
      "movups xmm15, [r11+0x290]\n\t"
      "ldmxcsr [r11+0x34]\n\t"
      "mov rbx, [r11+0x90]\n\t"
      "mov rbp, [r11+0xA0]\n\t"
      "mov rsi, [r11+0xA8]\n\t"
      "mov rdi, [r11+0xB0]\n\t"
      "mov r8, [r11+0xB8]\n\t"
      "mov r9, [r11+0xC0]\n\t"
      "mov r10, [r11+0xC8]\n\t"
      "mov r12, [r11+0xD8]\n\t"
      "mov r13, [r11+0xE0]\n\t"
      "mov r14, [r11+0xE8]\n\t"
      "mov r15, [r11+0xF0]\n\t"
      "mov rdx, [r11+0x88]\n\t"
      "mov rax, [r11+0x78]\n\t"
      "mov rcx, [r11+0x80]\n\t"
      "mov rsp, [r11+0x98]\n\t"
      "push qword ptr [r11+0xF8]\n\t"
      "mov r11, [r11+0xD0]\n\t"
      "ret\n\t"
      ".att_syntax\n");
}

// ---- the dispatcher -----------------------------------------------------------------------------------------------------

typedef struct {
  ULONG64 base;
  PRUNTIME_FUNCTION rf;
  PEXCEPTION_ROUTINE handler;
  PVOID handler_data;
  ULONG64 frame;                                  // the establisher frame
  ULONG64 pc;
  CONTEXT fctx;                                   // the frame's own context (where it is, at its call)
} step_t;

// Look at the frame `c` is in: describe it in *s, then move `c` to the caller. Returns 0 at the
// end of the stack.
static int step(CONTEXT *c, DWORD type, step_t *s) {
  ULONG64 base;
  PRUNTIME_FUNCTION rf;

  if (!rt_eh_find_module(c->Rip, &base)) {
    return 0;
  }
  s->pc = c->Rip;
  s->fctx = *c;
  s->handler = 0;
  s->handler_data = 0;
  s->frame = c->Rsp;
  s->base = base;
  rf = RtlLookupFunctionEntry(c->Rip, &base, 0);
  s->rf = rf;
  if (rf == 0) {                                      // a leaf function: the return address is on top of the stack
    c->Rip = *(DWORD64 *)(uintptr_t)c->Rsp;
    c->Rsp += 8;
    return 1;
  }
  s->handler = RtlVirtualUnwind(type, base, c->Rip, rf, c, &s->handler_data, &s->frame, 0);
  return 1;
}

eh_catch_t *rt_eh_catch_for(const CONTEXT *c) {
  eh_catch_t *r;

  if (c->Rip != (ULONG64)(uintptr_t)__winclone_catch_ret) {
    return 0;
  }
  for (r = *eh_catch_slot(); r != 0; r = r->next) {
    if (r->marker_rsp == c->Rsp) {
      return r;
    }
  }
  return 0;
}

// Is another running catch block (or the exception being dispatched) still using `obj`?
static int object_in_use(void *obj, const EXCEPTION_RECORD *rec) {
  eh_catch_t *r;

  if (rec != 0 && rec->ExceptionCode == CXX_EXCEPTION_CODE && rec->NumberParameters >= 3 && (void *)rec->ExceptionInformation[1] == obj) {
    return 1;
  }
  for (r = *eh_catch_slot(); r != 0; r = r->next) {
    if (r->obj == obj) {
      return 1;
    }
  }
  return 0;
}

// A catch block is being left by an exception: drop its record and destroy its exception unless
// the exception being thrown (a rethrow) or an outer catch block still owns it.
void rt_eh_leave_catch(eh_catch_t *r, const EXCEPTION_RECORD *rec) {
  eh_catch_t **pp;

  for (pp = eh_catch_slot(); *pp != 0 && *pp != r; pp = &(*pp)->next) {
  }
  if (*pp == r) {
    *pp = r->next;
  }
  if (!object_in_use(r->obj, rec)) {
    rt_eh_destroy_exception(r->obj, r->throw_info, r->base);
  }
}

void rt_eh_destroy_exception(void *obj, const void *throw_info, ULONG64 base) {
  if (throw_info != 0) {
    int dtor = ((const int *)throw_info)[1];            // ThrowInfo.pmfnUnwind

    if (dtor != 0) {
      ((void (*)(void *))(uintptr_t)(base + (ULONG64)(unsigned)dtor))(obj);
    }
  }
}

void rt_eh_release(void *obj, const void *throw_info, ULONG64 base) {
  if (!object_in_use(obj, 0)) {
    rt_eh_destroy_exception(obj, throw_info, base);
  }
}

static const char hexdigits[] = "0123456789ABCDEF";

static void put_hex(char *out, ULONG64 v, int digits) {
  int i;

  for (i = 0; i < digits; ++i) {
    out[i] = hexdigits[(v >> (4 * (digits - 1 - i))) & 15];
  }
  out[digits] = 0;
}

static LPTOP_LEVEL_EXCEPTION_FILTER g_unhandled_filter;

EXPORT LPTOP_LEVEL_EXCEPTION_FILTER WINAPI SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f) {
  LPTOP_LEVEL_EXCEPTION_FILTER old = g_unhandled_filter;

  g_unhandled_filter = f;
  return old;
}

EXPORT LONG WINAPI UnhandledExceptionFilter(struct _EXCEPTION_POINTERS *p) {
  (void)p;
  return EXCEPTION_CONTINUE_SEARCH;
}

static DECLSPEC_NORETURN void unhandled(EXCEPTION_RECORD *rec, CONTEXT *ctx) {
  char hex[20];

  if (g_unhandled_filter != 0) {
    EXCEPTION_POINTERS ep;

    ep.ExceptionRecord = rec;
    ep.ContextRecord = ctx;
    g_unhandled_filter(&ep);
  }
  if (rec->ExceptionCode == CXX_EXCEPTION_CODE) {
    rt_log("\r\nterminate called after throwing an exception that nothing catches.\r\n");
    ExitProcess(3);
  }
  rt_log("\r\nUnhandled exception 0x");
  put_hex(hex, rec->ExceptionCode, 8);
  rt_log(hex);
  rt_log(" at 0x");
  put_hex(hex, (ULONG64)(uintptr_t)rec->ExceptionAddress, 16);
  rt_log(hex);
  rt_log("\r\n");
  ExitProcess(rec->ExceptionCode);
  for (;;) {
  }
}

// Phase 1: find the frame that handles the exception.
static int search(eh_dispatch_t *d) {
  CONTEXT c = *d->ctx;
  step_t s;
  int n;

  d->rec->ExceptionFlags &= EXCEPTION_NONCONTINUABLE;
  for (n = 0; n < 8192 && step(&c, UNW_FLAG_EHANDLER, &s); ++n) {
    eh_catch_t *r;

    if (s.handler != 0) {
      DISPATCHER_CONTEXT dc;

      memset(&dc, 0, sizeof(dc));
      dc.ControlPc = s.pc;
      dc.ImageBase = s.base;
      dc.FunctionEntry = s.rf;
      dc.EstablisherFrame = s.frame;
      dc.ContextRecord = &s.fctx;
      dc.LanguageHandler = s.handler;
      dc.HandlerData = s.handler_data;
      s.handler(d->rec, (PVOID)(uintptr_t)s.frame, &s.fctx, &dc);
      if (d->found.kind != 0) {
        d->found.key = s.frame;
        if (d->found.frame == 0) {
          d->found.frame = s.frame;
        }
        return 1;
      }
    }
    while ((r = rt_eh_catch_for(&c)) != 0) {
      c = r->parent_up;                               // skip the dead frames between the throw and this catch block
    }
  }
  return 0;
}

// Phase 2: run the cleanups of the frames between the throw and the handler, then resume there.
static DECLSPEC_NORETURN void unwind_and_resume(eh_dispatch_t *d, eh_dispatch_t *prev) {
  CONTEXT c = *d->ctx;
  step_t s;
  EXCEPTION_RECORD *rec = d->rec;
  int n;

  for (n = 0; n < 8192 && step(&c, UNW_FLAG_UHANDLER, &s); ++n) {
    int target = s.rf != 0 && s.frame == d->found.key;
    eh_catch_t *r;

    if (s.handler != 0) {
      DISPATCHER_CONTEXT dc;

      memset(&dc, 0, sizeof(dc));
      dc.ControlPc = s.pc;
      dc.ImageBase = s.base;
      dc.FunctionEntry = s.rf;
      dc.EstablisherFrame = s.frame;
      dc.ContextRecord = &s.fctx;
      dc.LanguageHandler = s.handler;
      dc.HandlerData = s.handler_data;
      dc.TargetIp = target ? d->found.target_ip : 0;
      rec->ExceptionFlags = (rec->ExceptionFlags & EXCEPTION_NONCONTINUABLE) | EXCEPTION_UNWINDING | (target ? EXCEPTION_TARGET_UNWIND : 0);
      s.handler(rec, (PVOID)(uintptr_t)s.frame, &s.fctx, &dc);
    }
    if (target) {
      CONTEXT resume, up;

      if (d->found.kind == 2) {
        rt_eh_cxx_select(d, s.rf, &s.fctx, &c, &resume, &up);
      } else {
        resume = s.fctx;
        up = c;
      }
      *eh_cur_slot() = prev;
      if (d->found.kind == 1) {
        resume.Rip = d->found.target_ip;
      } else {
        resume.Rip = rt_eh_cxx_run_catch(d, &resume, &up);
      }
      RtlRestoreContext(&resume, 0);
    }
    while ((r = rt_eh_catch_for(&c)) != 0) {          // passing through a catch block: leave it
      c = r->parent_up;
      rt_eh_leave_catch(r, rec);
    }
  }
  unhandled(rec, d->ctx);
}

void rt_eh_dispatch(EXCEPTION_RECORD *rec, CONTEXT *ctx) {
  eh_dispatch_t d;
  eh_dispatch_t *prev = *eh_cur_slot();

  memset(&d, 0, sizeof(d));
  d.rec = rec;
  d.ctx = ctx;
  *eh_cur_slot() = &d;
  if (search(&d)) {
    unwind_and_resume(&d, prev);
  }
  *eh_cur_slot() = prev;
  unhandled(rec, ctx);
}

EXPORT VOID WINAPI RaiseException(DWORD code, DWORD flags, DWORD n, const ULONG_PTR *args) {
  EXCEPTION_RECORD rec;
  CONTEXT ctx;
  DWORD i;

  memset(&rec, 0, sizeof(rec));
  rec.ExceptionCode = code;
  rec.ExceptionFlags = flags & EXCEPTION_NONCONTINUABLE;
  rec.NumberParameters = n > EXCEPTION_MAXIMUM_PARAMETERS ? EXCEPTION_MAXIMUM_PARAMETERS : n;
  for (i = 0; i < rec.NumberParameters; ++i) {
    rec.ExceptionInformation[i] = args[i];
  }
  RtlCaptureContext(&ctx);
  rec.ExceptionAddress = (PVOID)(uintptr_t)ctx.Rip;
  rt_eh_dispatch(&rec, &ctx);
}

EXPORT VOID WINAPI RtlRaiseException(PEXCEPTION_RECORD rec) {
  CONTEXT ctx;

  RtlCaptureContext(&ctx);
  rec->ExceptionFlags &= EXCEPTION_NONCONTINUABLE;
  rt_eh_dispatch(rec, &ctx);
}

// Unwind to `target_frame` (0: all the way), running the frames' cleanups, and continue at `target_ip`.
EXPORT VOID WINAPI RtlUnwindEx(PVOID target_frame, PVOID target_ip, PEXCEPTION_RECORD rec, PVOID retval, PCONTEXT ctx, PUNWIND_HISTORY_TABLE hist) {
  CONTEXT c;
  step_t s;
  EXCEPTION_RECORD local;
  int n;

  (void)ctx;
  (void)hist;
  if (rec == 0) {
    memset(&local, 0, sizeof(local));
    local.ExceptionCode = 0xC0000027;                 // STATUS_UNWIND
    rec = &local;
  }
  RtlCaptureContext(&c);
  for (n = 0; n < 8192 && step(&c, UNW_FLAG_UHANDLER, &s); ++n) {
    int target = target_frame != 0 && s.rf != 0 && s.frame == (ULONG64)(uintptr_t)target_frame;
    eh_catch_t *r;

    if (s.handler != 0) {
      DISPATCHER_CONTEXT dc;

      memset(&dc, 0, sizeof(dc));
      dc.ControlPc = s.pc;
      dc.ImageBase = s.base;
      dc.FunctionEntry = s.rf;
      dc.EstablisherFrame = s.frame;
      dc.ContextRecord = &s.fctx;
      dc.LanguageHandler = s.handler;
      dc.HandlerData = s.handler_data;
      dc.TargetIp = target ? (ULONG64)(uintptr_t)target_ip : 0;
      rec->ExceptionFlags = (rec->ExceptionFlags & EXCEPTION_NONCONTINUABLE) | (target_frame != 0 ? EXCEPTION_UNWINDING : EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND) |
                            (target ? EXCEPTION_TARGET_UNWIND : 0);
      s.handler(rec, (PVOID)(uintptr_t)s.frame, &s.fctx, &dc);
    }
    if (target) {
      CONTEXT resume = s.fctx;

      resume.Rip = (ULONG64)(uintptr_t)target_ip;
      resume.Rax = (ULONG64)(uintptr_t)retval;
      RtlRestoreContext(&resume, 0);
    }
    while ((r = rt_eh_catch_for(&c)) != 0) {
      c = r->parent_up;
      rt_eh_leave_catch(r, rec);
    }
  }
  ExitProcess(rec->ExceptionCode);
}

EXPORT VOID WINAPI RtlUnwind(PVOID target_frame, PVOID target_ip, PEXCEPTION_RECORD rec, PVOID retval) {
  RtlUnwindEx(target_frame, target_ip, rec, retval, 0, 0);
}

EXPORT PVOID WINAPI RtlPcToFileHeader(PVOID pc, PVOID *base) {
  ULONG64 b = 0;

  rt_eh_find_module((ULONG64)(uintptr_t)pc, &b);
  if (base != 0) {
    *base = (PVOID)(uintptr_t)b;
  }
  return (PVOID)(uintptr_t)b;
}

EXPORT PVOID WINAPI AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER h) { (void)first; return (PVOID)h; }
EXPORT ULONG WINAPI RemoveVectoredExceptionHandler(PVOID h) { (void)h; return 1; }

// ---- __try / __except / __finally -----------------------------------------------------------------------------------------

typedef struct {
  ULONG begin, end, handler, jump_target;
} scope_record_t;

// __C_specific_handler: search phase: run the filters of the scopes around the call; unwind phase:
// run the __finally blocks, stopping at the __except block that took the exception.
EXPORT_SPECIAL EXCEPTION_DISPOSITION __C_specific_handler(struct _EXCEPTION_RECORD *rec, void *frame, struct _CONTEXT *ctx, struct _DISPATCHER_CONTEXT *dc) {
  const ULONG *table = (const ULONG *)dc->HandlerData;
  ULONG count = table[0], i;
  const scope_record_t *scopes = (const scope_record_t *)(table + 1);
  ULONG64 rva = dc->ControlPc - dc->ImageBase;
  eh_dispatch_t *d = *eh_cur_slot();

  (void)ctx;
  for (i = 0; i < count; ++i) {
    const scope_record_t *sc = &scopes[i];

    if (rva < sc->begin || rva >= sc->end) {
      continue;
    }
    if ((rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) == 0) {
      LONG verdict;

      if (sc->jump_target == 0) {
        continue;                                     // a __finally: nothing to decide
      }
      if (sc->handler == 1) {
        verdict = EXCEPTION_EXECUTE_HANDLER;
      } else {
        EXCEPTION_POINTERS ep;

        ep.ExceptionRecord = rec;
        ep.ContextRecord = d != 0 ? d->ctx : ctx;
        verdict = ((LONG (*)(EXCEPTION_POINTERS *, void *))(uintptr_t)(dc->ImageBase + sc->handler))(&ep, frame);
      }
      if (verdict > 0 && d != 0) {                    // EXCEPTION_EXECUTE_HANDLER
        d->found.kind = 1;
        d->found.target_ip = dc->ImageBase + sc->jump_target;
        return ExceptionContinueSearch;
      }
    } else {
      if ((rec->ExceptionFlags & EXCEPTION_TARGET_UNWIND) != 0 && sc->jump_target != 0 && dc->ImageBase + sc->jump_target == dc->TargetIp) {
        return ExceptionContinueSearch;               // the scope that handles it: stop here
      }
      if (sc->jump_target == 0 && sc->handler != 0) {
        ((void (*)(BOOLEAN, void *))(uintptr_t)(dc->ImageBase + sc->handler))(TRUE, frame);
      }
    }
  }
  return ExceptionContinueSearch;
}
