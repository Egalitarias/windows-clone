// C++ exceptions: _CxxThrowException and __CxxFrameHandler3, the language handler the compiler puts
// in the unwind info of every function that has try blocks or objects with destructors. The data it
// reads (FuncInfo, the unwind map, try blocks, catch handlers, ThrowInfo) is the compiler's
// "FH3" layout, with 32-bit RVAs relative to the image base. __CxxFrameHandler4 (the compressed
// layout cl.exe has used by default since Visual Studio 2019, unless /d2FH4- is given) is not
// supported; clang emits FH3.
//
// A throw raises exception 0xE06D7363 through the dispatcher in seh.c. In the search phase this
// handler looks for a matching catch in the frame's try blocks; in the unwind phase it runs the
// destructors (the unwind map's actions) of the objects the frame had constructed. The catch
// block itself is a funclet that rt_eh_cxx_run_catch calls once everything below it is unwound;
// it returns the address where the function carries on after the try/catch.
#include "eh.h"

typedef struct {
  int magic, max_state, disp_unwind_map;
  unsigned n_try;
  int disp_try_map;
  unsigned n_ip;
  int disp_ip_map, disp_unwind_help, disp_es_list, flags;
} func_info_t;

typedef struct { int to_state, action; } unwind_entry_t;
typedef struct { int try_low, try_high, catch_high, n_catches, disp_handlers; } try_block_t;
typedef struct { unsigned adjectives; int disp_type, disp_catch_obj, disp_handler, disp_frame; } handler_t;
typedef struct { int ip, state; } ip_state_t;
typedef struct { int attributes, disp_unwind, disp_compat, disp_catchables; } throw_info_t;
typedef struct { int n; int types[1]; } catchable_array_t;
typedef struct { unsigned properties; int disp_type, mdisp, pdisp, vdisp, size, disp_copy; } catchable_t;
typedef struct { void *vft, *spare; char name[1]; } type_desc_t;

#define TI_IS_CONST        1
#define TI_IS_VOLATILE     2
#define CT_IS_SIMPLE       1
#define CT_BY_REF_ONLY     2
#define CT_HAS_VBASE       4
#define HT_IS_CONST        1
#define HT_IS_VOLATILE     2
#define HT_IS_REFERENCE    8

#define RV(base, rva) ((const void *)(uintptr_t)((ULONG64)(base) + (ULONG64)(unsigned)(rva)))

// ---- calling a catch funclet ---------------------------------------------------------------------------
// rt_call_catch(funclet, frame, &marker_rsp, exception_object) calls the funclet as the compiler
// expects (rcx = the exception object, rdx = the frame of the function that owns the catch block)
// and returns its result, the continuation address. __winclone_catch_ret is where the funclet
// returns to: an exception thrown inside the block recognises it while unwinding.

__asm__(".intel_syntax noprefix\n"
        ".text\n"
        ".globl rt_call_catch\n"
        "rt_call_catch:\n"
        "  sub rsp, 40\n"
        "  mov rax, rcx\n"
        "  mov rcx, r9\n"
        "  mov [r8], rsp\n"
        "  call rax\n"
        ".globl __winclone_catch_ret\n"
        "__winclone_catch_ret:\n"
        "  add rsp, 40\n"
        "  ret\n"
        ".att_syntax\n");

void *rt_call_catch(void *funclet, ULONG64 frame, ULONG64 *marker_rsp, void *obj);

// ---- reading the compiler's tables ------------------------------------------------------------------

static int state_at(const func_info_t *fi, ULONG64 base, ULONG64 rva) {
  const ip_state_t *m = (const ip_state_t *)RV(base, fi->disp_ip_map);
  int state = -1;
  unsigned i;

  for (i = 0; i < fi->n_ip; ++i) {
    if ((unsigned)m[i].ip > rva) {
      break;
    }
    state = m[i].state;
  }
  return state;
}

// Run the destructors of the objects alive in `from`, down to (not including) state `to`.
static void unwind_states(const func_info_t *fi, ULONG64 base, ULONG64 frame, int from, int to) {
  const unwind_entry_t *um = (const unwind_entry_t *)RV(base, fi->disp_unwind_map);
  int guard = 0;

  while (from != to && from >= 0 && from < fi->max_state && guard++ < 4096) {
    const unwind_entry_t *e = &um[from];
    int next = e->to_state;

    if (e->action != 0) {
      ((void (*)(void *, void *))(uintptr_t)RV(base, e->action))(0, (void *)(uintptr_t)frame);
    }
    from = next;
  }
}

static int same_type(const type_desc_t *a, const type_desc_t *b) {
  return a == b || rt_strcmp(a->name, b->name) == 0;
}

// Does catch handler `h` take the thrown exception? Returns the catchable type that matches (for a
// catch (...), which has none, *any is set).
static const catchable_t *match(const handler_t *h, const throw_info_t *ti, ULONG64 tbase, ULONG64 base, int *any) {
  const catchable_array_t *ca;
  int i;

  *any = 0;
  if (h->disp_type == 0) {
    *any = 1;
    return 0;
  }
  ca = (const catchable_array_t *)RV(tbase, ti->disp_catchables);
  for (i = 0; i < ca->n; ++i) {
    const catchable_t *ct = (const catchable_t *)RV(tbase, ca->types[i]);

    if (!same_type((const type_desc_t *)RV(tbase, ct->disp_type), (const type_desc_t *)RV(base, h->disp_type))) {
      continue;
    }
    if ((ct->properties & CT_BY_REF_ONLY) && !(h->adjectives & HT_IS_REFERENCE)) {
      continue;
    }
    if ((h->adjectives & HT_IS_REFERENCE) && (((ti->attributes & TI_IS_CONST) && !(h->adjectives & HT_IS_CONST)) ||
                                             ((ti->attributes & TI_IS_VOLATILE) && !(h->adjectives & HT_IS_VOLATILE)))) {
      continue;
    }
    return ct;
  }
  return 0;
}

// The address of the (sub)object a catchable type refers to inside the thrown object.
static void *this_ptr(void *obj, const catchable_t *ct) {
  char *p = (char *)obj;

  if (ct->pdisp < 0) {
    return p + ct->mdisp;
  }
  {
    const char *vbtable = *(const char **)(p + ct->pdisp);

    return p + ct->pdisp + *(const int *)(vbtable + ct->vdisp) + ct->mdisp;
  }
}

// ---- the language handler ------------------------------------------------------------------------------------

// When `frame` belongs to a catch funclet, the function the funclet is part of has a different
// frame: the compiler stored its address in the funclet's frame, `disp_frame` bytes from `frame`.
// Everything the handler does with the frame (variables, destructors) refers to the owner's.
static ULONG64 owner_frame(const func_info_t *fi, ULONG64 base, ULONG64 frame, const DISPATCHER_CONTEXT *dc) {
  const try_block_t *tbs = (const try_block_t *)RV(base, fi->disp_try_map);
  unsigned i;
  int k;

  for (i = 0; i < fi->n_try; ++i) {
    const handler_t *hs = (const handler_t *)RV(base, tbs[i].disp_handlers);

    for (k = 0; k < tbs[i].n_catches; ++k) {
      if ((ULONG)hs[k].disp_handler == dc->FunctionEntry->BeginAddress && hs[k].disp_frame != 0) {
        return *(const ULONG64 *)(uintptr_t)(frame + (ULONG64)(LONG64)hs[k].disp_frame);
      }
    }
  }
  return frame;
}

EXPORT_SPECIAL EXCEPTION_DISPOSITION __CxxFrameHandler3(EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx, DISPATCHER_CONTEXT *dc) {
  const func_info_t *fi = (const func_info_t *)RV(dc->ImageBase, *(const int *)dc->HandlerData);
  ULONG64 base = dc->ImageBase;
  int state = state_at(fi, base, dc->ControlPc - base);
  eh_dispatch_t *d = *eh_cur_slot();
  ULONG64 owner = owner_frame(fi, base, (ULONG64)(uintptr_t)frame, dc);
  unsigned i;

  (void)ctx;
  if ((rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) != 0) {
    int target = -1;

    if ((rec->ExceptionFlags & EXCEPTION_TARGET_UNWIND) != 0 && d != 0 && d->found.kind == 2 && d->found.key == (ULONG64)(uintptr_t)frame) {
      target = d->found.target_state;
    }
    unwind_states(fi, base, owner, state, target);
    return ExceptionContinueSearch;
  }
  if (rec->ExceptionCode != CXX_EXCEPTION_CODE || rec->NumberParameters < 3 || d == 0) {
    return ExceptionContinueSearch;
  }
  {
    const throw_info_t *ti = (const throw_info_t *)(uintptr_t)rec->ExceptionInformation[2];
    ULONG64 tbase = rec->NumberParameters >= 4 ? rec->ExceptionInformation[3] : base;
    const try_block_t *tbs = (const try_block_t *)RV(base, fi->disp_try_map);

    if (ti == 0) {
      return ExceptionContinueSearch;
    }
    for (i = 0; i < fi->n_try; ++i) {
      const try_block_t *tb = &tbs[i];
      const handler_t *hs = (const handler_t *)RV(base, tb->disp_handlers);
      int k;

      if (state < tb->try_low || state > tb->try_high) {
        continue;
      }
      for (k = 0; k < tb->n_catches; ++k) {
        int any;
        const catchable_t *ct = match(&hs[k], ti, tbase, base, &any);

        if (ct != 0 || any) {
          d->found.kind = 2;
          d->found.frame = owner;
          d->found.func_info = fi;
          d->found.base = base;
          d->found.try_block = tb;
          d->found.handler = &hs[k];
          d->found.catchable = ct;
          d->found.target_state = tb->try_low;
          return ExceptionContinueSearch;               // the dispatcher sees `found`
        }
      }
    }
    // Nothing here catches it. A function with an empty exception specification (noexcept, throw())
    // may not let a C++ exception leave it.
    if (fi->disp_es_list != 0 && *(const int *)RV(base, fi->disp_es_list) == 0) {
      rt_log("[EH] empty exception specification\r\n");
      __std_terminate();
    }
  }
  return ExceptionContinueSearch;
}

EXPORT_SPECIAL EXCEPTION_DISPOSITION __CxxFrameHandler4(EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx, DISPATCHER_CONTEXT *dc) {
  static int warned;

  (void)rec; (void)frame; (void)ctx; (void)dc;
  if (!warned) {
    warned = 1;
    rt_log("\r\n[EH] this program uses the compressed C++ exception tables (__CxxFrameHandler4): rebuild it with /d2FH4- or clang\r\n");
  }
  return ExceptionContinueSearch;
}

// ---- running a catch block ---------------------------------------------------------------------------------------

// The try block whose catch funclet starts at `begin` (a function entry), if there is one.
static const try_block_t *funclet_try(const func_info_t *fi, ULONG64 base, ULONG begin) {
  const try_block_t *tbs = (const try_block_t *)RV(base, fi->disp_try_map);
  unsigned i;
  int k;

  for (i = 0; i < fi->n_try; ++i) {
    const handler_t *hs = (const handler_t *)RV(base, tbs[i].disp_handlers);

    for (k = 0; k < tbs[i].n_catches; ++k) {
      if ((ULONG)hs[k].disp_handler == begin) {
        return &tbs[i];
      }
    }
  }
  return 0;
}

// A catch block can contain try blocks of its own, so the frame the handler was found in may be a
// catch funclet, and the handling try block inside it or outside it. A try block inside has states
// above the funclet's own try block; one outside (in the function that owns the funclet, or an
// enclosing funclet) does not. If outside, execution resumes in the owner, and the catch blocks
// being left are dropped, innermost first.
void rt_eh_cxx_select(eh_dispatch_t *d, const RUNTIME_FUNCTION *rf, const CONTEXT *fctx, const CONTEXT *after, CONTEXT *resume, CONTEXT *up) {
  const func_info_t *fi = (const func_info_t *)d->found.func_info;
  const try_block_t *handling = (const try_block_t *)d->found.try_block;
  ULONG64 base = d->found.base;
  ULONG begin = rf != 0 ? rf->BeginAddress : 0;

  *resume = *fctx;
  *up = *after;
  for (;;) {
    const try_block_t *own = funclet_try(fi, base, begin);
    eh_catch_t *r;
    ULONG64 ignored;
    PRUNTIME_FUNCTION owner_rf;

    if (own == 0 || handling->try_low > own->try_high) {
      break;                                          // resume in this frame
    }
    r = rt_eh_catch_for(up);
    if (r == 0) {
      break;
    }
    *resume = r->parent;
    *up = r->parent_up;
    rt_eh_leave_catch(r, d->rec);
    owner_rf = RtlLookupFunctionEntry(resume->Rip, &ignored, 0);
    if (owner_rf == 0) {
      break;
    }
    begin = owner_rf->BeginAddress;
  }
}

// Everything below the handling function has been unwound. Copy the exception into the catch
// parameter, run the catch block (a funclet) and return the address to continue at.
ULONG64 rt_eh_cxx_run_catch(eh_dispatch_t *d, const CONTEXT *parent, const CONTEXT *parent_up) {
  const handler_t *h = (const handler_t *)d->found.handler;
  const catchable_t *ct = (const catchable_t *)d->found.catchable;
  void *obj = (void *)(uintptr_t)d->rec->ExceptionInformation[1];
  const throw_info_t *ti = (const throw_info_t *)(uintptr_t)d->rec->ExceptionInformation[2];
  ULONG64 tbase = d->rec->NumberParameters >= 4 ? d->rec->ExceptionInformation[3] : d->found.base;
  ULONG64 frame = d->found.frame;
  eh_catch_t rc;
  void *cont;

  if (h->disp_type != 0 && h->disp_catch_obj != 0 && ct != 0) {
    void *dest = (void *)(uintptr_t)(frame + (ULONG64)(LONG64)h->disp_catch_obj), *src = this_ptr(obj, ct);

    if (h->adjectives & HT_IS_REFERENCE) {
      *(void **)dest = src;
    } else if (ct->properties & CT_IS_SIMPLE) {
      memcpy(dest, src, (size_t)ct->size);
    } else if (ct->disp_copy != 0) {
      if (ct->properties & CT_HAS_VBASE) {
        ((void (*)(void *, void *, int))(uintptr_t)RV(tbase, ct->disp_copy))(dest, src, 1);
      } else {
        ((void (*)(void *, void *))(uintptr_t)RV(tbase, ct->disp_copy))(dest, src);
      }
    } else {
      memcpy(dest, src, (size_t)ct->size);
    }
  }
  rc.next = *eh_catch_slot();
  rc.parent = *parent;
  rc.parent_up = *parent_up;
  rc.obj = obj;
  rc.throw_info = ti;
  rc.base = tbase;
  rc.marker_rsp = 0;
  *eh_catch_slot() = &rc;
  cont = rt_call_catch((void *)(uintptr_t)RV(d->found.base, h->disp_handler), frame, &rc.marker_rsp, obj);
  *eh_catch_slot() = rc.next;
  rt_eh_release(obj, ti, tbase);
  return (ULONG64)(uintptr_t)cont;
}

// ---- throwing ---------------------------------------------------------------------------------------------------------

EXPORT_SPECIAL DECLSPEC_NORETURN void _CxxThrowException(void *obj, const void *ti) {
  EXCEPTION_RECORD rec;
  CONTEXT ctx;
  ULONG64 base = rt_exe_base();

  if (obj == 0 && ti == 0) {                           // "throw;": the exception being handled
    eh_catch_t *r = *eh_catch_slot();

    if (r == 0) {
      rt_log("[EH] rethrow with no exception\r\n");
      __std_terminate();
    }
    obj = r->obj;
    ti = r->throw_info;
    base = r->base;
  } else if (ti != 0) {
    rt_eh_find_module((ULONG64)(uintptr_t)ti, &base);
  }
  memset(&rec, 0, sizeof(rec));
  rec.ExceptionCode = CXX_EXCEPTION_CODE;
  rec.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
  rec.NumberParameters = 4;
  rec.ExceptionInformation[0] = CXX_MAGIC;
  rec.ExceptionInformation[1] = (ULONG_PTR)obj;
  rec.ExceptionInformation[2] = (ULONG_PTR)ti;
  rec.ExceptionInformation[3] = (ULONG_PTR)base;
  RtlCaptureContext(&ctx);
  rec.ExceptionAddress = (PVOID)(uintptr_t)ctx.Rip;
  rt_eh_dispatch(&rec, &ctx);
  rt_log("[EH] dispatch returned\r\n");
  __std_terminate();
}

// std::exception keeps its message in a __std_exception_data.
typedef struct {
  const char *what;
  char do_free;
} std_exception_data_t;

EXPORT_SPECIAL void __cdecl __std_exception_copy(const std_exception_data_t *from, std_exception_data_t *to) {
  if (from->do_free) {
    size_t n = rt_strlen(from->what) + 1;
    char *p = (char *)rt_alloc(n);

    if (p != 0) {
      memcpy(p, from->what, n);
      to->what = p;
      to->do_free = 1;
      return;
    }
  }
  to->what = from->what;
  to->do_free = 0;
}

EXPORT_SPECIAL void __cdecl __std_exception_destroy(std_exception_data_t *data) {
  if (data->do_free) {
    rt_free((void *)data->what);
  }
  data->what = 0;
  data->do_free = 0;
}

