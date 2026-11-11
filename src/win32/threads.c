// Threads and synchronisation for the Win32 runtime.
//
// The kernel gives a program extra threads (SYS_THREAD_CREATE) and one blocking primitive, a futex
// (SYS_FUTEX_WAIT / SYS_FUTEX_WAKE). Everything else is built here, in user mode: thread objects,
// events, mutexes, semaphores, critical sections, SRW locks, condition variables, one-time
// initialisation, WaitOnAddress, thread-local storage. All waiting goes through one futex word,
// g_seq, that every state change increments: a waiter reads it, checks its condition, and sleeps
// until it changes. That wakes more threads than necessary, which only costs a re-check.
#include "internal.h"

static volatile LONG g_seq;

static void notify(void) {
  __sync_fetch_and_add(&g_seq, 1);
  sc3(KS_FUTEX_WAKE, (sl_t)&g_seq, 0, 0);
}

// Sleep until g_seq is no longer `seq`, for at most `ms` (INFINITE: no limit). Returns 0 when it
// changed (or on a spurious wake-up), 1 on a timeout.
static int wait_seq(LONG seq, DWORD ms) {
  sl_t r = sc3(KS_FUTEX_WAIT, (sl_t)&g_seq, (sl_t)seq, ms == INFINITE ? (sl_t)-1 : (sl_t)ms);

  return r == -110 ? 1 : 0;
}

// ---- objects ----------------------------------------------------------------------------------------------

typedef struct {
  int kind;                       // H_THREAD, H_EVENT, H_MUTEX, H_SEM
  volatile LONG refs;
  // events
  volatile LONG signaled;
  int manual;
  // mutexes
  volatile DWORD owner;
  volatile LONG count;
  // semaphores
  volatile LONG sem, sem_max;
  // threads
  volatile LONG done, suspended;
  DWORD tid, exit_code;
  LPTHREAD_START_ROUTINE fn;
  LPVOID arg;
} obj_t;

static obj_t *obj_new(int kind) {
  obj_t *o = (obj_t *)rt_alloc(sizeof(*o));

  if (o != 0) {
    memset(o, 0, sizeof(*o));
    o->kind = kind;
    o->refs = 1;
  }
  return o;
}

static void obj_release(obj_t *o) {
  if (__sync_sub_and_fetch(&o->refs, 1) == 0) {
    rt_free(o);
  }
}

void rt_obj_close(int kind, void *obj) {
  (void)kind;
  if (obj != 0) {
    obj_release((obj_t *)obj);
  }
}

static obj_t *obj_of(HANDLE h) {
  int kind = 0;
  obj_t *o = (obj_t *)rt_handle_obj(h, &kind);

  return (o != 0 && kind >= H_THREAD) ? o : 0;
}

static HANDLE obj_handle(obj_t *o) {
  HANDLE h;

  if (o == 0) {
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return 0;
  }
  h = rt_handle_new(o->kind, o);
  if (h == INVALID_HANDLE_VALUE) {
    rt_free(o);
    return 0;
  }
  return h;
}

// Is the object signaled? With `take`, also consume it (an auto-reset event, a semaphore count,
// a free mutex).
static int obj_ready(obj_t *o, int take) {
  switch (o->kind) {
    case H_THREAD:
      return o->done != 0;
    case H_EVENT:
      if (o->manual || !take) {
        return o->signaled != 0;
      }
      return __sync_val_compare_and_swap(&o->signaled, 1, 0) == 1;
    case H_MUTEX: {
      DWORD me = rt_tid();

      if (o->owner == me) {
        if (take) {
          ++o->count;
        }
        return 1;
      }
      if (!take) {
        return o->owner == 0;
      }
      if (__sync_val_compare_and_swap(&o->owner, 0, me) == 0) {
        o->count = 1;
        return 1;
      }
      return 0;
    }
    case H_SEM:
      for (;;) {
        LONG v = o->sem;

        if (v <= 0) {
          return 0;
        }
        if (!take || __sync_val_compare_and_swap(&o->sem, v, v - 1) == v) {
          return 1;
        }
      }
    default:
      return 1;
  }
}

static DWORD remaining(ULONGLONG deadline, DWORD ms) {
  ULONGLONG now;

  if (ms == INFINITE) {
    return INFINITE;
  }
  now = GetTickCount64();
  return now >= deadline ? 0 : (DWORD)(deadline - now);
}

EXPORT DWORD WINAPI WaitForMultipleObjects(DWORD n, const HANDLE *handles, BOOL all, DWORD ms) {
  obj_t *objs[64];
  ULONGLONG deadline = ms == INFINITE ? 0 : GetTickCount64() + ms;
  DWORD i;

  if (n == 0 || n > 64) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return WAIT_FAILED;
  }
  for (i = 0; i < n; ++i) {
    int kind = 0;
    void *ob = rt_handle_obj(handles[i], &kind);

    if (kind == 0) {
      SetLastError(ERROR_INVALID_HANDLE);
      return WAIT_FAILED;
    }
    objs[i] = kind >= H_THREAD ? (obj_t *)ob : 0;       // files and the console count as always ready
  }
  for (;;) {
    LONG seq = g_seq;

    if (all) {
      int ok = 1;

      for (i = 0; i < n; ++i) {
        if (objs[i] != 0 && !obj_ready(objs[i], 0)) {
          ok = 0;
        }
      }
      if (ok) {
        for (i = 0; i < n; ++i) {
          if (objs[i] != 0 && !obj_ready(objs[i], 1)) {
            ok = 0;                                   // lost a race: try again
          }
        }
        if (ok) {
          return WAIT_OBJECT_0;
        }
      }
    } else {
      for (i = 0; i < n; ++i) {
        if (objs[i] == 0 || obj_ready(objs[i], 1)) {
          return WAIT_OBJECT_0 + i;
        }
      }
    }
    {
      DWORD left = remaining(deadline, ms);

      if (ms != INFINITE && left == 0) {
        return WAIT_TIMEOUT;
      }
      wait_seq(seq, left);
    }
  }
}

EXPORT DWORD WINAPI WaitForSingleObject(HANDLE h, DWORD ms) {
  if (h == (HANDLE)(intptr_t)-2 || h == (HANDLE)(intptr_t)-1) {
    return WAIT_TIMEOUT;                              // the current thread / process never finish
  }
  return WaitForMultipleObjects(1, &h, FALSE, ms);
}

EXPORT DWORD WINAPI WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable) { (void)alertable; return WaitForSingleObject(h, ms); }
EXPORT DWORD WINAPI WaitForMultipleObjectsEx(DWORD n, const HANDLE *h, BOOL all, DWORD ms, BOOL alertable) {
  (void)alertable;
  return WaitForMultipleObjects(n, h, all, ms);
}

EXPORT VOID WINAPI Sleep(DWORD ms) {
  if (ms == INFINITE) {
    for (;;) {
      sc3(KS_SLEEP, 3600000, 0, 0);
    }
  }
  if (ms == 0) {
    sc3(KS_YIELD, 0, 0, 0);
    return;
  }
  sc3(KS_SLEEP, (sl_t)ms, 0, 0);
}
EXPORT DWORD WINAPI SleepEx(DWORD ms, BOOL alertable) { (void)alertable; Sleep(ms); return 0; }
EXPORT BOOL WINAPI SwitchToThread(void) { sc3(KS_YIELD, 0, 0, 0); return TRUE; }

// ---- events, mutexes, semaphores ------------------------------------------------------------------------------

EXPORT HANDLE WINAPI CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCSTR name) {
  obj_t *o = obj_new(H_EVENT);

  (void)sa; (void)name;
  if (o != 0) {
    o->manual = manual != 0;
    o->signaled = initial != 0;
  }
  return obj_handle(o);
}
EXPORT HANDLE WINAPI CreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCWSTR name) {
  return CreateEventA(sa, manual, initial, 0);
}
EXPORT HANDLE WINAPI CreateEventExW(LPSECURITY_ATTRIBUTES sa, LPCWSTR name, DWORD flags, DWORD access) {
  (void)access;
  return CreateEventA(sa, (flags & CREATE_EVENT_MANUAL_RESET) != 0, (flags & CREATE_EVENT_INITIAL_SET) != 0, 0);
}

EXPORT BOOL WINAPI SetEvent(HANDLE h) {
  obj_t *o = obj_of(h);

  if (o == 0 || o->kind != H_EVENT) {
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  o->signaled = 1;
  notify();
  return TRUE;
}

EXPORT BOOL WINAPI ResetEvent(HANDLE h) {
  obj_t *o = obj_of(h);

  if (o == 0 || o->kind != H_EVENT) {
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  o->signaled = 0;
  return TRUE;
}

EXPORT BOOL WINAPI PulseEvent(HANDLE h) {
  if (!SetEvent(h)) {
    return FALSE;
  }
  sc3(KS_YIELD, 0, 0, 0);
  return ResetEvent(h);
}

EXPORT HANDLE WINAPI CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL initial_owner, LPCSTR name) {
  obj_t *o = obj_new(H_MUTEX);

  (void)sa; (void)name;
  if (o != 0 && initial_owner) {
    o->owner = rt_tid();
    o->count = 1;
  }
  return obj_handle(o);
}
EXPORT HANDLE WINAPI CreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL initial_owner, LPCWSTR name) {
  return CreateMutexA(sa, initial_owner, 0);
}

EXPORT BOOL WINAPI ReleaseMutex(HANDLE h) {
  obj_t *o = obj_of(h);

  if (o == 0 || o->kind != H_MUTEX) {
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  if (o->owner != rt_tid()) {
    SetLastError(ERROR_NOT_OWNER);
    return FALSE;
  }
  if (--o->count == 0) {
    o->owner = 0;
    notify();
  }
  return TRUE;
}

EXPORT HANDLE WINAPI CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG max, LPCSTR name) {
  obj_t *o;

  (void)sa; (void)name;
  if (max <= 0 || initial < 0 || initial > max) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return 0;
  }
  o = obj_new(H_SEM);
  if (o != 0) {
    o->sem = initial;
    o->sem_max = max;
  }
  return obj_handle(o);
}
EXPORT HANDLE WINAPI CreateSemaphoreW(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG max, LPCWSTR name) {
  return CreateSemaphoreA(sa, initial, max, 0);
}

EXPORT BOOL WINAPI ReleaseSemaphore(HANDLE h, LONG n, LPLONG previous) {
  obj_t *o = obj_of(h);

  if (o == 0 || o->kind != H_SEM || n <= 0) {
    SetLastError(o == 0 || o->kind != H_SEM ? ERROR_INVALID_HANDLE : ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  for (;;) {
    LONG v = o->sem;

    if (v > o->sem_max - n) {
      SetLastError(ERROR_TOO_MANY_POSTS);
      return FALSE;
    }
    if (__sync_val_compare_and_swap(&o->sem, v, v + n) == v) {
      if (previous != 0) {
        *previous = v;
      }
      break;
    }
  }
  notify();
  return TRUE;
}

// ---- threads ----------------------------------------------------------------------------------------------------

// Where every new thread starts (the kernel enters here, in the new thread): the start routine and
// its argument are in the thread block.
EXPORT_SPECIAL void __winclone_thread_start(void) {
  void (*fn)(void *);
  void *arg;

  __asm__ volatile("movq %%gs:0x300, %0\n\tmovq %%gs:0x308, %1" : "=r"(fn), "=r"(arg));
  fn(arg);
  sc3(KS_THREAD_EXIT, 0, 0, 0);
  for (;;) {
  }
}

// A thread killed while it runs user code (the process is ending) is redirected here by the
// kernel, with rax and rdi already set up for the exit system call.
EXPORT_SPECIAL __attribute__((naked)) void __winclone_die(void) {
  __asm__ volatile("syscall\n\tud2");
}

static void thread_finish(obj_t *o, DWORD code) {
  o->exit_code = code;
  o->done = 1;
  notify();
  obj_release(o);
}

static void thread_main(void *arg) {
  obj_t *o = (obj_t *)arg;

  __asm__ volatile("movq %0, %%gs:0x318" : : "r"(o));
  while (o->suspended) {
    LONG seq = g_seq;

    if (o->suspended) {
      wait_seq(seq, INFINITE);
    }
  }
  thread_finish(o, o->fn(o->arg));
}

EXPORT HANDLE WINAPI CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE start, LPVOID param, DWORD flags, LPDWORD id) {
  obj_t *o = obj_new(H_THREAD);
  HANDLE h;
  sl_t tid;

  (void)sa; (void)stack;
  if (o == 0 || start == 0) {
    if (o != 0) {
      rt_free(o);
    }
    SetLastError(o == 0 ? ERROR_NOT_ENOUGH_MEMORY : ERROR_INVALID_PARAMETER);
    return 0;
  }
  o->refs = 2;                                        // the handle and the running thread
  o->fn = start;
  o->arg = param;
  o->suspended = (flags & CREATE_SUSPENDED) != 0;
  h = rt_handle_new(H_THREAD, o);
  if (h == INVALID_HANDLE_VALUE) {
    rt_free(o);
    return 0;
  }
  tid = sc3(KS_THREAD_CREATE, (sl_t)(uintptr_t)__winclone_thread_start, (sl_t)(uintptr_t)thread_main, (sl_t)(uintptr_t)o);
  if (tid < 0) {
    rt_handle_free(h);
    rt_free(o);
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return 0;
  }
  o->tid = (DWORD)tid;
  if (id != 0) {
    *id = (DWORD)tid;
  }
  return h;
}

EXPORT DECLSPEC_NORETURN VOID WINAPI ExitThread(DWORD code) {
  obj_t *o;

  __asm__ volatile("movq %%gs:0x318, %0" : "=r"(o));
  if (o != 0) {
    __asm__ volatile("movq $0, %%gs:0x318" : : : "memory");
    thread_finish(o, code);
  }
  sc3(KS_THREAD_EXIT, (sl_t)code, 0, 0);               // the main thread: the program ends
  for (;;) {
  }
}

EXPORT HANDLE WINAPI GetCurrentThread(void) { return (HANDLE)(intptr_t)-2; }
EXPORT DWORD WINAPI GetCurrentThreadId(void) { return rt_tid(); }
EXPORT DWORD WINAPI GetThreadId(HANDLE h) {
  obj_t *o = obj_of(h);

  return o != 0 && o->kind == H_THREAD ? o->tid : (h == (HANDLE)(intptr_t)-2 ? rt_tid() : 0);
}

EXPORT BOOL WINAPI GetExitCodeThread(HANDLE h, LPDWORD code) {
  obj_t *o = obj_of(h);

  if (o == 0 || o->kind != H_THREAD) {
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  *code = o->done ? o->exit_code : STILL_ACTIVE;
  return TRUE;
}

EXPORT DWORD WINAPI ResumeThread(HANDLE h) {
  obj_t *o = obj_of(h);
  DWORD prev;

  if (o == 0 || o->kind != H_THREAD) {
    SetLastError(ERROR_INVALID_HANDLE);
    return (DWORD)-1;
  }
  prev = o->suspended ? 1 : 0;
  o->suspended = 0;
  notify();
  return prev;
}

EXPORT DWORD WINAPI SuspendThread(HANDLE h) { (void)h; SetLastError(ERROR_NOT_SUPPORTED); return (DWORD)-1; }
EXPORT BOOL WINAPI TerminateThread(HANDLE h, DWORD code) { (void)h; (void)code; SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
EXPORT BOOL WINAPI SetThreadPriority(HANDLE h, int p) { (void)h; (void)p; return TRUE; }
EXPORT int WINAPI GetThreadPriority(HANDLE h) { (void)h; return 0; }
EXPORT DWORD_PTR WINAPI SetThreadAffinityMask(HANDLE h, DWORD_PTR m) { (void)h; (void)m; return 1; }
EXPORT HRESULT WINAPI SetThreadDescription(HANDLE h, PCWSTR d) { (void)h; (void)d; return 0; }
EXPORT DWORD WINAPI GetCurrentProcessorNumber(void) { return 0; }

// The C runtime's names for the same things.
EXPORT_SPECIAL uintptr_t _beginthreadex(void *sa, unsigned stack, unsigned (__stdcall *start)(void *), void *arg, unsigned flags, unsigned *id) {
  DWORD tid = 0;
  HANDLE h = CreateThread(0, stack, (LPTHREAD_START_ROUTINE)start, arg, flags, &tid);

  if (id != 0) {
    *id = tid;
  }
  return (uintptr_t)h;
}
EXPORT_SPECIAL DECLSPEC_NORETURN void _endthreadex(unsigned code) { ExitThread(code); }
EXPORT_SPECIAL DECLSPEC_NORETURN void _endthread(void) { ExitThread(0); }

typedef struct {
  void (__cdecl *fn)(void *);
  void *arg;
} bt_t;

static DWORD WINAPI beginthread_main(LPVOID p) {
  bt_t b = *(bt_t *)p;

  rt_free(p);
  b.fn(b.arg);
  return 0;
}

EXPORT_SPECIAL uintptr_t _beginthread(void (__cdecl *fn)(void *), unsigned stack, void *arg) {
  bt_t *b = (bt_t *)rt_alloc(sizeof(*b));
  HANDLE h;

  if (b == 0) {
    return (uintptr_t)-1;
  }
  b->fn = fn;
  b->arg = arg;
  h = CreateThread(0, stack, beginthread_main, b, 0, 0);
  if (h == 0) {
    rt_free(b);
    return (uintptr_t)-1;
  }
  return (uintptr_t)h;
}

// ---- thread-local storage -----------------------------------------------------------------------------------------

static volatile LONG g_tls_used[2];                   // 64 slots

static void **tls_slots(void) { return (void **)(rt_teb() + 0xC00); }

EXPORT DWORD WINAPI TlsAlloc(void) {
  DWORD i;

  for (i = 0; i < 64; ++i) {
    volatile LONG *word = &g_tls_used[i / 32];
    LONG bit = 1L << (i % 32), old = *word;

    while ((old & bit) == 0) {
      LONG seen = __sync_val_compare_and_swap(word, old, old | bit);

      if (seen == old) {
        tls_slots()[i] = 0;
        return i;
      }
      old = seen;
    }
  }
  SetLastError(ERROR_NO_MORE_ITEMS);
  return TLS_OUT_OF_INDEXES;
}

EXPORT BOOL WINAPI TlsFree(DWORD i) {
  if (i >= 64) {
    return FALSE;
  }
  __sync_fetch_and_and(&g_tls_used[i / 32], ~(1L << (i % 32)));
  return TRUE;
}
EXPORT LPVOID WINAPI TlsGetValue(DWORD i) { SetLastError(0); return i < 64 ? tls_slots()[i] : 0; }
EXPORT BOOL WINAPI TlsSetValue(DWORD i, LPVOID v) { if (i < 64) { tls_slots()[i] = v; return TRUE; } return FALSE; }

// ---- critical sections, SRW locks, condition variables, one-time initialisation -------------------------------------

EXPORT VOID WINAPI InitializeCriticalSection(LPCRITICAL_SECTION c) { memset(c, 0, sizeof(*c)); }
EXPORT BOOL WINAPI InitializeCriticalSectionEx(LPCRITICAL_SECTION c, DWORD n, DWORD f) { (void)n; (void)f; memset(c, 0, sizeof(*c)); return TRUE; }
EXPORT BOOL WINAPI InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION c, DWORD n) { (void)n; memset(c, 0, sizeof(*c)); return TRUE; }
EXPORT DWORD WINAPI SetCriticalSectionSpinCount(LPCRITICAL_SECTION c, DWORD n) { (void)c; (void)n; return 0; }
EXPORT VOID WINAPI DeleteCriticalSection(LPCRITICAL_SECTION c) { (void)c; }

// LockCount is 1 while the section is held (an all-zero section is a free one), OwningThread the
// holder's thread id, RecursionCount how many times it entered.
EXPORT BOOL WINAPI TryEnterCriticalSection(LPCRITICAL_SECTION c) {
  DWORD me = rt_tid();

  if ((DWORD)(uintptr_t)c->OwningThread == me && c->LockCount != 0) {
    ++c->RecursionCount;
    return TRUE;
  }
  if (__sync_val_compare_and_swap(&c->LockCount, 0, 1) != 0) {
    return FALSE;
  }
  c->OwningThread = (HANDLE)(uintptr_t)me;
  c->RecursionCount = 1;
  return TRUE;
}

EXPORT VOID WINAPI EnterCriticalSection(LPCRITICAL_SECTION c) {
  for (;;) {
    LONG seq = g_seq;

    if (TryEnterCriticalSection(c)) {
      return;
    }
    wait_seq(seq, INFINITE);
  }
}

EXPORT VOID WINAPI LeaveCriticalSection(LPCRITICAL_SECTION c) {
  if (--c->RecursionCount == 0) {
    c->OwningThread = 0;
    __sync_lock_release(&c->LockCount);
    notify();
  }
}

// SRWLOCK: bit 0 is the exclusive flag, the rest counts shared holders (2 per holder).
EXPORT VOID WINAPI InitializeSRWLock(PSRWLOCK l) { l->Ptr = 0; }

EXPORT BOOLEAN WINAPI TryAcquireSRWLockExclusive(PSRWLOCK l) {
  return __sync_val_compare_and_swap((volatile LONG_PTR *)&l->Ptr, 0, 1) == 0;
}

EXPORT BOOLEAN WINAPI TryAcquireSRWLockShared(PSRWLOCK l) {
  for (;;) {
    LONG_PTR v = (LONG_PTR)l->Ptr;

    if (v & 1) {
      return FALSE;
    }
    if (__sync_val_compare_and_swap((volatile LONG_PTR *)&l->Ptr, v, v + 2) == v) {
      return TRUE;
    }
  }
}

EXPORT VOID WINAPI AcquireSRWLockExclusive(PSRWLOCK l) {
  for (;;) {
    LONG seq = g_seq;

    if (TryAcquireSRWLockExclusive(l)) {
      return;
    }
    wait_seq(seq, INFINITE);
  }
}

EXPORT VOID WINAPI AcquireSRWLockShared(PSRWLOCK l) {
  for (;;) {
    LONG seq = g_seq;

    if (TryAcquireSRWLockShared(l)) {
      return;
    }
    wait_seq(seq, INFINITE);
  }
}

EXPORT VOID WINAPI ReleaseSRWLockExclusive(PSRWLOCK l) {
  __sync_lock_release((volatile LONG_PTR *)&l->Ptr);
  notify();
}

EXPORT VOID WINAPI ReleaseSRWLockShared(PSRWLOCK l) {
  __sync_fetch_and_sub((volatile LONG_PTR *)&l->Ptr, 2);
  notify();
}

// CONDITION_VARIABLE: a counter in its first four bytes, bumped by every wake-up.
static volatile LONG *cv_word(PCONDITION_VARIABLE cv) { return (volatile LONG *)&cv->Ptr; }

EXPORT VOID WINAPI InitializeConditionVariable(PCONDITION_VARIABLE cv) { cv->Ptr = 0; }
EXPORT VOID WINAPI WakeConditionVariable(PCONDITION_VARIABLE cv) { __sync_fetch_and_add(cv_word(cv), 1); notify(); }
EXPORT VOID WINAPI WakeAllConditionVariable(PCONDITION_VARIABLE cv) { WakeConditionVariable(cv); }

EXPORT BOOL WINAPI SleepConditionVariableCS(PCONDITION_VARIABLE cv, PCRITICAL_SECTION cs, DWORD ms) {
  LONG seen = *cv_word(cv);
  sl_t r;
  LONG rec = cs->RecursionCount;

  cs->RecursionCount = 1;
  LeaveCriticalSection(cs);
  r = sc3(KS_FUTEX_WAIT, (sl_t)(uintptr_t)cv_word(cv), (sl_t)seen, ms == INFINITE ? (sl_t)-1 : (sl_t)ms);
  EnterCriticalSection(cs);
  cs->RecursionCount = rec;
  if (r == -110) {
    SetLastError(ERROR_TIMEOUT);
    return FALSE;
  }
  return TRUE;
}

EXPORT BOOL WINAPI SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK l, DWORD ms, ULONG flags) {
  LONG seen = *cv_word(cv);
  sl_t r;
  int shared = (flags & CONDITION_VARIABLE_LOCKMODE_SHARED) != 0;

  if (shared) {
    ReleaseSRWLockShared(l);
  } else {
    ReleaseSRWLockExclusive(l);
  }
  r = sc3(KS_FUTEX_WAIT, (sl_t)(uintptr_t)cv_word(cv), (sl_t)seen, ms == INFINITE ? (sl_t)-1 : (sl_t)ms);
  if (shared) {
    AcquireSRWLockShared(l);
  } else {
    AcquireSRWLockExclusive(l);
  }
  if (r == -110) {
    SetLastError(ERROR_TIMEOUT);
    return FALSE;
  }
  return TRUE;
}

// One-time initialisation: Ptr is 0 (not started), 1 (running) or 2 (done).
EXPORT BOOL WINAPI InitOnceExecuteOnce(PINIT_ONCE once, PINIT_ONCE_FN fn, PVOID param, LPVOID *ctx) {
  volatile LONG_PTR *state = (volatile LONG_PTR *)&once->Ptr;

  for (;;) {
    LONG seq = g_seq;
    LONG_PTR old = __sync_val_compare_and_swap(state, 0, 1);

    if (old == 2) {
      if (ctx != 0) {
        *ctx = 0;
      }
      return TRUE;
    }
    if (old == 0) {
      BOOL ok = fn(once, param, ctx);

      *state = ok ? 2 : 0;
      notify();
      return ok;
    }
    wait_seq(seq, INFINITE);
  }
}

EXPORT BOOL WINAPI WaitOnAddress(volatile VOID *addr, PVOID cmp, SIZE_T size, DWORD ms) {
  ULONGLONG deadline = ms == INFINITE ? 0 : GetTickCount64() + ms;

  for (;;) {
    LONG seq = g_seq;
    DWORD left;

    if (memcmp((const void *)addr, cmp, size) != 0) {
      return TRUE;
    }
    left = remaining(deadline, ms);
    if (ms != INFINITE && left == 0) {
      SetLastError(ERROR_TIMEOUT);
      return FALSE;
    }
    wait_seq(seq, left);
  }
}
EXPORT VOID WINAPI WakeByAddressSingle(PVOID addr) { (void)addr; notify(); }
EXPORT VOID WINAPI WakeByAddressAll(PVOID addr) { (void)addr; notify(); }
