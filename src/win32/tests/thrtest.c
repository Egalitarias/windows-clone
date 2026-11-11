// Threads and synchronisation, built with the dynamic C runtime like crttest.c. Exit code: the
// number of failed checks (0 = all good). With an argument it ends the program in a different way
// while other threads are busy: "exit" (a thread calls exit(5)), "fault" (a thread writes to
// address 0) or "ret" (the main thread returns while the others spin or wait).
#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static volatile LONG g_fail, g_checks;
#define CHECK(cond) do { InterlockedIncrement(&g_checks); if (!(cond)) { InterlockedIncrement(&g_fail); printf("THR FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static __declspec(thread) int tls_value = 100;
static DWORD g_tls_index;

// ---- join and exit codes ----
static DWORD WINAPI return_code(LPVOID arg) { return (DWORD)(ULONG_PTR)arg + 1; }
static DWORD WINAPI exit_code_thread(LPVOID arg) { ExitThread(77); return 0; }

static void test_join(void) {
  DWORD id = 0, code = 0;
  HANDLE h = CreateThread(NULL, 0, return_code, (LPVOID)41, 0, &id);

  CHECK(h != NULL && id != 0 && id != GetCurrentThreadId());
  CHECK(WaitForSingleObject(h, 5000) == WAIT_OBJECT_0);
  CHECK(GetExitCodeThread(h, &code) && code == 42);
  CloseHandle(h);
  h = CreateThread(NULL, 0, exit_code_thread, NULL, 0, NULL);
  CHECK(WaitForSingleObject(h, 5000) == WAIT_OBJECT_0);
  CHECK(GetExitCodeThread(h, &code) && code == 77);
  CloseHandle(h);
}

// ---- critical section and interlocked counters ----
static CRITICAL_SECTION g_cs;
static long g_plain;
static volatile LONG g_inter;

static DWORD WINAPI counter_thread(LPVOID arg) {
  int i;

  for (i = 0; i < 20000; ++i) {
    EnterCriticalSection(&g_cs);
    ++g_plain;
    LeaveCriticalSection(&g_cs);
    InterlockedIncrement(&g_inter);
  }
  return 0;
}

static void test_counters(void) {
  HANDLE h[4];
  int i;

  InitializeCriticalSection(&g_cs);
  for (i = 0; i < 4; ++i) {
    h[i] = CreateThread(NULL, 0, counter_thread, NULL, 0, NULL);
  }
  CHECK(WaitForMultipleObjects(4, h, TRUE, 20000) == WAIT_OBJECT_0);
  CHECK(g_plain == 80000);
  CHECK(g_inter == 80000);
  for (i = 0; i < 4; ++i) {
    CloseHandle(h[i]);
  }
  CHECK(TryEnterCriticalSection(&g_cs) && TryEnterCriticalSection(&g_cs));
  LeaveCriticalSection(&g_cs);
  LeaveCriticalSection(&g_cs);
  DeleteCriticalSection(&g_cs);
}

// ---- events ----
static HANDLE g_ev_go, g_ev_done;
static DWORD WINAPI event_thread(LPVOID arg) {
  int i;

  for (i = 0; i < 50; ++i) {
    WaitForSingleObject(g_ev_go, INFINITE);        // auto-reset: one wake-up per SetEvent
    SetEvent(g_ev_done);
  }
  return 0;
}

static void test_events(void) {
  HANDLE t;
  int i, ok = 1;
  HANDLE manual = CreateEventA(NULL, TRUE, FALSE, NULL);

  g_ev_go = CreateEventA(NULL, FALSE, FALSE, NULL);
  g_ev_done = CreateEventA(NULL, FALSE, FALSE, NULL);
  CHECK(g_ev_go != NULL && g_ev_done != NULL);
  CHECK(WaitForSingleObject(g_ev_go, 0) == WAIT_TIMEOUT);
  t = CreateThread(NULL, 0, event_thread, NULL, 0, NULL);
  for (i = 0; i < 50; ++i) {
    SetEvent(g_ev_go);
    if (WaitForSingleObject(g_ev_done, 5000) != WAIT_OBJECT_0) {
      ok = 0;
    }
  }
  CHECK(ok);
  CHECK(WaitForSingleObject(t, 5000) == WAIT_OBJECT_0);
  CHECK(WaitForSingleObject(manual, 0) == WAIT_TIMEOUT);
  SetEvent(manual);
  CHECK(WaitForSingleObject(manual, 0) == WAIT_OBJECT_0 && WaitForSingleObject(manual, 0) == WAIT_OBJECT_0);
  ResetEvent(manual);
  CHECK(WaitForSingleObject(manual, 50) == WAIT_TIMEOUT);
  CloseHandle(t);
  CloseHandle(manual);
  CloseHandle(g_ev_go);
  CloseHandle(g_ev_done);
}

// ---- mutex and semaphore: a bounded producer/consumer ----
static HANDLE g_mutex, g_items, g_slots;
static int g_queue[8], g_head, g_tail;
static long g_sum;

static DWORD WINAPI producer(LPVOID arg) {
  int i;

  for (i = 1; i <= 200; ++i) {
    WaitForSingleObject(g_slots, INFINITE);
    WaitForSingleObject(g_mutex, INFINITE);
    g_queue[g_tail++ % 8] = i;
    ReleaseMutex(g_mutex);
    ReleaseSemaphore(g_items, 1, NULL);
  }
  return 0;
}

static DWORD WINAPI consumer(LPVOID arg) {
  int i;

  for (i = 0; i < 200; ++i) {
    int v;

    WaitForSingleObject(g_items, INFINITE);
    WaitForSingleObject(g_mutex, INFINITE);
    v = g_queue[g_head++ % 8];
    ReleaseMutex(g_mutex);
    ReleaseSemaphore(g_slots, 1, NULL);
    g_sum += v;
  }
  return 0;
}

static void test_queue(void) {
  HANDLE t[2];

  g_mutex = CreateMutexA(NULL, FALSE, NULL);
  g_items = CreateSemaphoreA(NULL, 0, 8, NULL);
  g_slots = CreateSemaphoreA(NULL, 8, 8, NULL);
  CHECK(g_mutex && g_items && g_slots);
  CHECK(WaitForSingleObject(g_mutex, 0) == WAIT_OBJECT_0 && WaitForSingleObject(g_mutex, 0) == WAIT_OBJECT_0);   // recursive
  ReleaseMutex(g_mutex);
  ReleaseMutex(g_mutex);
  CHECK(!ReleaseMutex(g_mutex));                       // not owned any more
  t[0] = CreateThread(NULL, 0, producer, NULL, 0, NULL);
  t[1] = CreateThread(NULL, 0, consumer, NULL, 0, NULL);
  CHECK(WaitForMultipleObjects(2, t, TRUE, 30000) == WAIT_OBJECT_0);
  CHECK(g_sum == 200 * 201 / 2);
  CHECK(!ReleaseSemaphore(g_slots, 1, NULL));          // already at the maximum
  CloseHandle(t[0]);
  CloseHandle(t[1]);
  CloseHandle(g_mutex);
  CloseHandle(g_items);
  CloseHandle(g_slots);
}

// ---- thread-local storage, errno, last error ----
static volatile LONG g_tls_ok;

static DWORD WINAPI tls_thread(LPVOID arg) {
  int me = (int)(ULONG_PTR)arg, i;

  if (tls_value == 100) {
    InterlockedIncrement(&g_tls_ok);                   // a fresh copy of the initial value
  }
  tls_value = me;
  TlsSetValue(g_tls_index, (LPVOID)(ULONG_PTR)(me * 10));
  SetLastError(me);
  errno = me;
  for (i = 0; i < 20; ++i) {
    DWORD last;

    Sleep(1);
    last = GetLastError();                              // TlsGetValue clears it, so look first
    if (tls_value != me || last != (DWORD)me || errno != me || (int)(ULONG_PTR)TlsGetValue(g_tls_index) != me * 10) {
      return 1;
    }
    SetLastError(me);
  }
  return 0;
}

static void test_tls(void) {
  HANDLE t[3];
  DWORD code, i;

  g_tls_index = TlsAlloc();
  CHECK(g_tls_index != TLS_OUT_OF_INDEXES);
  TlsSetValue(g_tls_index, (LPVOID)(ULONG_PTR)7);
  tls_value = 5;
  SetLastError(1234);
  errno = 4321;
  for (i = 0; i < 3; ++i) {
    t[i] = CreateThread(NULL, 0, tls_thread, (LPVOID)(ULONG_PTR)(i + 1), 0, NULL);
  }
  CHECK(WaitForMultipleObjects(3, t, TRUE, 10000) == WAIT_OBJECT_0);
  for (i = 0; i < 3; ++i) {
    CHECK(GetExitCodeThread(t[i], &code) && code == 0);
    CloseHandle(t[i]);
  }
  CHECK(g_tls_ok == 3);
  CHECK(GetLastError() == 1234 && errno == 4321);
  CHECK(tls_value == 5 && (int)(ULONG_PTR)TlsGetValue(g_tls_index) == 7);
  TlsFree(g_tls_index);
}

// ---- the heap and printf from several threads at once ----
static volatile LONG g_heap_bad;

static DWORD WINAPI heap_thread(LPVOID arg) {
  int id = (int)(ULONG_PTR)arg, i, j;
  char *blocks[32];
  char text[64];

  memset(blocks, 0, sizeof(blocks));
  for (i = 0; i < 400; ++i) {
    int k = (i * 7 + id) % 32;
    size_t n = 16 + (size_t)((i * 37 + id * 11) % 200);

    if (blocks[k] != NULL) {
      for (j = 0; j < 16; ++j) {
        if ((unsigned char)blocks[k][j] != (unsigned char)(id * 16 + k)) {
          InterlockedIncrement(&g_heap_bad);
        }
      }
      free(blocks[k]);
    }
    blocks[k] = (char *)malloc(n);
    if (blocks[k] == NULL) {
      InterlockedIncrement(&g_heap_bad);
      continue;
    }
    memset(blocks[k], id * 16 + k, n);
    sprintf(text, "%d:%d %.3f %e", id, i, i / 7.0, id * 1.5e10);
    if (strncmp(text, "", 0) != 0 || strchr(text, ':') == NULL) {
      InterlockedIncrement(&g_heap_bad);
    }
  }
  for (i = 0; i < 32; ++i) {
    free(blocks[i]);
  }
  return 0;
}

static void test_heap(void) {
  HANDLE t[4];
  int i;

  for (i = 0; i < 4; ++i) {
    t[i] = (HANDLE)_beginthreadex(NULL, 0, (unsigned (__stdcall *)(void *))heap_thread, (void *)(ULONG_PTR)(i + 1), 0, NULL);
  }
  CHECK(WaitForMultipleObjects(4, t, TRUE, 60000) == WAIT_OBJECT_0);
  CHECK(g_heap_bad == 0);
  for (i = 0; i < 4; ++i) {
    CloseHandle(t[i]);
  }
}

// ---- SRW lock, condition variable, one-time initialisation ----
static SRWLOCK g_srw = SRWLOCK_INIT;
static CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;
static int g_ready, g_value;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static volatile LONG g_once_runs;

static BOOL CALLBACK once_fn(PINIT_ONCE o, PVOID p, PVOID *c) { InterlockedIncrement(&g_once_runs); Sleep(20); return TRUE; }

static DWORD WINAPI cv_thread(LPVOID arg) {
  InitOnceExecuteOnce(&g_once, once_fn, NULL, NULL);
  Sleep(30);
  AcquireSRWLockExclusive(&g_srw);
  g_value = 99;
  g_ready = 1;
  ReleaseSRWLockExclusive(&g_srw);
  WakeAllConditionVariable(&g_cv);
  return 0;
}

static void test_cv(void) {
  HANDLE t = CreateThread(NULL, 0, cv_thread, NULL, 0, NULL);
  HANDLE t2 = CreateThread(NULL, 0, cv_thread, NULL, 0, NULL);
  int got = 0;

  AcquireSRWLockExclusive(&g_srw);
  while (!g_ready) {
    if (!SleepConditionVariableSRW(&g_cv, &g_srw, 5000, 0)) {
      break;
    }
  }
  got = g_ready && g_value == 99;
  ReleaseSRWLockExclusive(&g_srw);
  CHECK(got);
  WaitForSingleObject(t, 5000);
  WaitForSingleObject(t2, 5000);
  CHECK(g_once_runs == 1);
  CloseHandle(t);
  CloseHandle(t2);
  AcquireSRWLockShared(&g_srw);
  CHECK(!TryAcquireSRWLockExclusive(&g_srw) && TryAcquireSRWLockShared(&g_srw));
  ReleaseSRWLockShared(&g_srw);
  ReleaseSRWLockShared(&g_srw);
  CHECK(TryAcquireSRWLockExclusive(&g_srw));
  ReleaseSRWLockExclusive(&g_srw);
}

// ---- timing and suspended threads ----
static volatile LONG g_started;
static DWORD WINAPI start_thread(LPVOID arg) { g_started = 1; return 0; }
static DWORD WINAPI sleeper(LPVOID arg) { Sleep(150); return 5; }

static void test_timing(void) {
  DWORD t0, el, code;
  HANDLE s, h;

  h = CreateThread(NULL, 0, sleeper, NULL, 0, NULL);
  t0 = GetTickCount();
  CHECK(WaitForSingleObject(h, 30) == WAIT_TIMEOUT);
  el = GetTickCount() - t0;
  CHECK(el >= 25 && el < 140);
  CHECK(GetExitCodeThread(h, &code) && code == STILL_ACTIVE);
  CHECK(WaitForSingleObject(h, 5000) == WAIT_OBJECT_0);
  el = GetTickCount() - t0;
  CHECK(el >= 140 && el < 1000);
  CloseHandle(h);
  s = CreateThread(NULL, 0, start_thread, NULL, CREATE_SUSPENDED, NULL);
  Sleep(60);
  CHECK(g_started == 0);
  CHECK(ResumeThread(s) == 1);
  CHECK(WaitForSingleObject(s, 5000) == WAIT_OBJECT_0 && g_started == 1);
  CloseHandle(s);
}

// ---- many threads, one after another ----
static DWORD WINAPI quick(LPVOID arg) { return (DWORD)(ULONG_PTR)arg; }

static void test_many(void) {
  int i, ok = 1;

  for (i = 0; i < 40; ++i) {
    DWORD code = 0;
    HANDLE h = CreateThread(NULL, 0, quick, (LPVOID)(ULONG_PTR)i, 0, NULL);

    if (h == NULL || WaitForSingleObject(h, 5000) != WAIT_OBJECT_0 || !GetExitCodeThread(h, &code) || code != (DWORD)i) {
      ok = 0;
    }
    if (h != NULL) {
      CloseHandle(h);
    }
  }
  CHECK(ok);
}

// ---- ways to end the program with other threads still running ----
static DWORD WINAPI spinner(LPVOID arg) {
  volatile unsigned long long n = 0;

  for (;;) {
    ++n;
  }
}
static DWORD WINAPI waiter(LPVOID arg) { WaitForSingleObject((HANDLE)arg, INFINITE); return 0; }
static DWORD WINAPI sleeper_forever(LPVOID arg) { Sleep(INFINITE); return 0; }
static DWORD WINAPI exiter(LPVOID arg) { Sleep(50); printf("a thread calls exit(5)\n"); exit(5); return 0; }
static DWORD WINAPI crasher(LPVOID arg) { Sleep(50); printf("a thread writes to address 0\n"); *(volatile int *)0 = 1; return 0; }

static void start_busy_threads(void) {
  HANDLE never = CreateEventA(NULL, TRUE, FALSE, NULL);

  CreateThread(NULL, 0, spinner, NULL, 0, NULL);
  CreateThread(NULL, 0, spinner, NULL, 0, NULL);
  CreateThread(NULL, 0, waiter, never, 0, NULL);
  CreateThread(NULL, 0, sleeper_forever, NULL, 0, NULL);
}

int main(int argc, char **argv) {
  printf("thrtest started, argc=%d\n", argc);
  if (argc > 1) {
    start_busy_threads();
    if (strcmp(argv[1], "exit") == 0) {
      CreateThread(NULL, 0, exiter, NULL, 0, NULL);
      Sleep(5000);                                      // killed by the exit() in the thread
      return 99;
    }
    if (strcmp(argv[1], "fault") == 0) {
      CreateThread(NULL, 0, crasher, NULL, 0, NULL);
      Sleep(5000);
      return 99;
    }
    Sleep(100);
    printf("main returns with 4 threads still running\n");
    return 3;
  }
  puts("-- join"); test_join();
  puts("-- counters"); test_counters();
  puts("-- events"); test_events();
  puts("-- queue"); test_queue();
  puts("-- tls"); test_tls();
  puts("-- heap"); test_heap();
  puts("-- cv"); test_cv();
  puts("-- timing"); test_timing();
  puts("-- many"); test_many();
  printf("thrtest: %ld checks, %ld failures\n", (long)g_checks, (long)g_fail);
  return (int)g_fail;
}
