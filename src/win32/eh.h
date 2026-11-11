#ifndef WIN32RT_EH_H
#define WIN32RT_EH_H

// Exception handling for the Win32 runtime: the x64 unwinder and dispatcher (seh.c) and the C++
// language support, __CxxFrameHandler3 and _CxxThrowException (cxxeh.c). See docs/WIN32.md.
#include "internal.h"

// The x64 unwind info the compiler emits (not in the SDK headers).
enum { UWOP_PUSH_NONVOL = 0, UWOP_ALLOC_LARGE, UWOP_ALLOC_SMALL, UWOP_SET_FPREG, UWOP_SAVE_NONVOL, UWOP_SAVE_NONVOL_FAR,
       UWOP_EPILOG, UWOP_SPARE_CODE, UWOP_SAVE_XMM128, UWOP_SAVE_XMM128_FAR, UWOP_PUSH_MACHFRAME };
typedef union {
  struct {
    BYTE CodeOffset;
    BYTE UnwindOp : 4;
    BYTE OpInfo : 4;
  };
  USHORT FrameOffset;
} UNWIND_CODE;
typedef struct {
  BYTE Version : 3;
  BYTE Flags : 5;
  BYTE SizeOfProlog;
  BYTE CountOfCodes;
  BYTE FrameRegister : 4;
  BYTE FrameOffset : 4;
  UNWIND_CODE UnwindCode[1];
} UNWIND_INFO;
#ifndef UNW_FLAG_EHANDLER
#define UNW_FLAG_EHANDLER  1
#define UNW_FLAG_UHANDLER  2
#define UNW_FLAG_CHAININFO 4
#endif

#define CXX_EXCEPTION_CODE 0xE06D7363u            // 'msc': what _CxxThrowException raises
#define CXX_MAGIC          0x19930520u

// A catch block that is running. Its funclet was called from rt_call_catch (cxxeh.c), so an
// exception thrown in the block, which unwinds the funclet's frame, arrives at the marker address
// __winclone_catch_ret; there the walk continues with the function that owns the block
// (`parent_up`: its caller), because the frames between the throw and the catch are dead.
typedef struct eh_catch {
  struct eh_catch *next;
  ULONG64 marker_rsp;                             // RSP in rt_call_catch just before the funclet was called
  CONTEXT parent;                                 // the owning function at its call site (where it resumes)
  CONTEXT parent_up;                              // its caller
  void *obj;                                      // the exception object being handled
  const void *throw_info;
  ULONG64 base;                                   // the module the throw info's RVAs are relative to
} eh_catch_t;

// What a frame's language handler found during the search phase.
typedef struct {
  int kind;                                       // 0 nothing yet, 1 an __except block, 2 a C++ catch
  ULONG64 key;                                    // the frame the dispatcher found the handler in (as RtlVirtualUnwind sees it)
  ULONG64 frame;                                  // the establisher frame of the function that handles it (differs from
                                                  // `key` when that frame is a catch funclet: the owner's frame)
  ULONG64 target_ip;                              // kind 1: the __except block
  const void *func_info;                          // kind 2: what the catch is and where
  ULONG64 base;
  const void *try_block;
  const void *handler;
  const void *catchable;
  int target_state;                               // unwind the function's objects down to this state
} eh_found_t;

typedef struct eh_dispatch {
  EXCEPTION_RECORD *rec;
  CONTEXT *ctx;
  eh_found_t found;
} eh_dispatch_t;

// Per-thread: the exception being dispatched and the stack of running catch blocks (TEB fields).
static inline eh_dispatch_t **eh_cur_slot(void) { return (eh_dispatch_t **)(rt_teb() + 0x320); }
static inline eh_catch_t **eh_catch_slot(void) { return (eh_catch_t **)(rt_teb() + 0x328); }

// seh.c
void rt_eh_dispatch(EXCEPTION_RECORD *rec, CONTEXT *ctx);
int rt_eh_find_module(ULONG64 addr, ULONG64 *base);
void rt_eh_destroy_exception(void *obj, const void *throw_info, ULONG64 base);
void rt_eh_release(void *obj, const void *throw_info, ULONG64 base);      // destroy it unless a running catch block still owns it
extern char __winclone_catch_ret[];
DECLSPEC_NORETURN void __std_terminate(void);        // crt.c

eh_catch_t *rt_eh_catch_for(const CONTEXT *c);                         // the running catch block whose funclet `c` just returned to
void rt_eh_leave_catch(eh_catch_t *r, const EXCEPTION_RECORD *rec);   // drop it (an exception leaves its block)

// cxxeh.c
ULONG64 rt_eh_cxx_run_catch(eh_dispatch_t *d, const CONTEXT *parent, const CONTEXT *parent_up);
// The handler was found in the frame whose entry is `rf` (context `fctx`; `after` is its caller's).
// Choose where execution resumes: that frame, or, if it is a catch funclet and the handling try
// block is outside it, the function that owns the funclet (dropping the catch blocks left).
void rt_eh_cxx_select(eh_dispatch_t *d, const RUNTIME_FUNCTION *rf, const CONTEXT *fctx, const CONTEXT *after, CONTEXT *resume, CONTEXT *up);

#endif
