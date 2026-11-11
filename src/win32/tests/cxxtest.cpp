// C++ exceptions and structured exception handling, built like crttest.c (dynamic CRT) but as C++.
// Exit code: the number of failed checks. With an argument it ends in a different way: "uncaught"
// (a throw nothing catches: terminate, exit code 3), "noexcept" (a noexcept function throws: 3).
#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <string.h>
#include <exception>
#include <new>

static int g_fail, g_checks;
static char g_log[4096];

#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_fail; printf("EH FAIL line %d: %s\n", __LINE__, #cond); } } while (0)
#define CHECK_LOG(expect) do { ++g_checks; if (strcmp(g_log, expect) != 0) { ++g_fail; printf("EH FAIL line %d: log is \"%s\", wanted \"%s\"\n", __LINE__, g_log, expect); } g_log[0] = 0; } while (0)

static __declspec(thread) bool t_quiet;                 // worker threads do not write the shared log
static void L(const char *s) { if (!t_quiet) strcat(g_log, s); }

struct Tracer {
  const char *name;
  explicit Tracer(const char *n) : name(n) { L("+"); L(n); L(" "); }
  Tracer(const Tracer &o) : name(o.name) { L("copy-"); L(name); L(" "); }
  ~Tracer() { L("-"); L(name); L(" "); }
};

// ---- throw through frames with destructors ----
static void deep(void) { Tracer a("a"); throw 42; }
static void middle(void) { Tracer b("b"); deep(); L("not-reached "); }

static void test_basic(void) {
  int caught = 0;

  try {
    Tracer c("c");
    middle();
  } catch (int v) {
    L("catch ");
    caught = v;
  }
  CHECK(caught == 42);
  CHECK_LOG("+c +b +a -a -b -c catch ");
}

// ---- class types, bases, order of the handlers ----
struct Base { virtual ~Base() { L("~Base "); } int x = 1; };
struct Derived : Base { ~Derived() { L("~Derived "); } int y = 2; };
struct Other { int z; };

static void test_classes(void) {
  int which = 0;

  try { throw Derived(); }
  catch (const Other &) { which = 1; }
  catch (Base &b) { which = 2; CHECK(b.x == 1); }
  catch (...) { which = 3; }
  CHECK(which == 2);
  CHECK_LOG("~Derived ~Base ");                            // the exception object, destroyed once

  try { throw Other{7}; }
  catch (Other o) { which = 10 + o.z; }
  CHECK(which == 17);

  try { throw "text"; }
  catch (const char *s) { which = (int)strlen(s); }
  CHECK(which == 4);

  try { throw 2.5; }
  catch (int) { which = 1; }
  catch (double d) { which = d == 2.5 ? 100 : 101; }
  CHECK(which == 100);

  try { throw Tracer("t"); }
  catch (Tracer t) { L("by-value "); }
  g_log[0] = 0;
}

struct MyErr : std::exception {
  const char *what() const noexcept override { return "my error"; }
};

static void test_std_exception(void) {
  bool ok = false;

  try { throw MyErr(); }
  catch (const std::exception &e) { ok = strcmp(e.what(), "my error") == 0; }
  CHECK(ok);
  ok = false;
  try { throw std::bad_alloc(); }
  catch (const std::exception &e) { ok = e.what() != nullptr; }
  CHECK(ok);
  ok = false;
  try { throw std::exception(); }
  catch (std::exception e) { ok = true; }                 // copies it (__std_exception_copy)
  CHECK(ok);
}

// ---- rethrow and nesting ----
static void test_rethrow(void) {
  int r = 0;

  try {
    try { throw 5; }
    catch (...) { L("inner "); throw; }
  } catch (int v) { L("outer "); r = v; }
  CHECK(r == 5);
  CHECK_LOG("inner outer ");

  try {
    try { throw 5; }
    catch (int) { Tracer t("in-catch"); throw 6; }
  } catch (int v) { r = v; L("outer2 "); }
  CHECK(r == 6);
  CHECK_LOG("+in-catch -in-catch outer2 ");

  // a catch block that throws something the same try's other handlers must not see
  r = 0;
  try {
    try { throw 1; }
    catch (int) { throw 2.0; }
    catch (double) { r = -1; }
  } catch (double) { r = 2; }
  CHECK(r == 2);

  // thrown inside a catch block and caught by an outer try in the same function
  r = 0;
  try {
    try {
      try { throw 'a'; }
      catch (char) { throw 3; }
    } catch (int v) { r = v; throw 4L; }
  } catch (long v) { r += (int)v; }
  CHECK(r == 7);

  // the exception object survives the nested catch
  r = 0;
  try {
    try { throw MyErr(); }
    catch (const std::exception &e) {
      try { throw; } catch (const MyErr &m) { r = strcmp(m.what(), "my error") == 0 ? 1 : 0; }
      r += strcmp(e.what(), "my error") == 0 ? 10 : 0;
    }
  } catch (...) { r = -1; }
  CHECK(r == 11);
}

// ---- objects under construction ----
struct Member {
  Tracer t;
  Member(const char *n, bool fail) : t(n) { if (fail) throw 9; }
};
struct Holder {
  Member a, b, c;
  Holder() : a("m1", false), b("m2", false), c("m3", true) { L("body "); }
};

static void test_construction(void) {
  int r = 0;

  try { Holder h; L("made "); }
  catch (int v) { r = v; }
  CHECK(r == 9);
  CHECK_LOG("+m1 +m2 +m3 -m3 -m2 -m1 ");

  try { Tracer *arr = new Tracer[2]{Tracer("n1"), Tracer("n2")}; delete[] arr; }
  catch (...) { }
  g_log[0] = 0;
}

static void test_many(void) {
  int caught = 0, i;

  for (i = 0; i < 3000; ++i) {
    try {
      Tracer t("x");
      g_log[0] = 0;
      if (i % 3 == 0) throw i;
      if (i % 3 == 1) throw MyErr();
      throw "s";
    } catch (int v) { caught += v == i; }
    catch (const std::exception &) { ++caught; }
    catch (const char *) { ++caught; }
    g_log[0] = 0;
  }
  CHECK(caught == 3000);
}

// ---- new throws std::bad_alloc ----
static void test_new(void) {
  bool bad = false;

  try {
    static char *volatile sink;
    char *p = new char[0x7FFFFFFFFFFFull];

    sink = p;                                               // keep the compiler from dropping the new/delete pair
    delete[] p;
  } catch (const std::bad_alloc &) { bad = true; }
  CHECK(bad);
}

// ---- threads ----
static volatile LONG g_thread_ok;

static void throws_in_thread(int k) {
  Tracer t("th");
  if (k & 1) throw k;
  throw MyErr();
}

static DWORD WINAPI thread_proc(LPVOID arg) {
  int id = (int)(ULONG_PTR)arg, i, ok = 1;

  t_quiet = true;
  for (i = 0; i < 300; ++i) {
    try { throws_in_thread(id + i); ok = 0; }
    catch (int v) { if (v != id + i) ok = 0; }
    catch (const std::exception &e) { if (strcmp(e.what(), "my error") != 0) ok = 0; }
  }
  if (ok) InterlockedIncrement(&g_thread_ok);
  return 0;
}

static void test_threads(void) {
  HANDLE h[3];
  int i;

  for (i = 0; i < 3; ++i) h[i] = CreateThread(NULL, 0, thread_proc, (LPVOID)(ULONG_PTR)(i * 1000), 0, NULL);
  CHECK(WaitForMultipleObjects(3, h, TRUE, 60000) == WAIT_OBJECT_0);
  CHECK(g_thread_ok == 3);
  for (i = 0; i < 3; ++i) CloseHandle(h[i]);
  g_log[0] = 0;
}

// ---- structured exception handling ----
static int g_seh_code;

static int filter(DWORD code) { g_seh_code = (int)code; return code == 0xE0001234 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH; }

static void seh_inner(void) {
  Tracer t("seh-obj");
  RaiseException(0xE0001234, 0, 0, NULL);
  L("not-reached ");
}

static int seh_except_finally(void) {
  int r = 0;

  __try {
    __try { seh_inner(); }
    __finally { L("finally "); }
  } __except (filter(GetExceptionCode())) {
    r = 1;
    L("except ");
  }
  return r;
}

static void throw_11(void) { throw 11; }

// A C++ exception passes through a __try/__finally, and an __except that does not want it.
static void seh_passes_cxx(void) {
  __try {
    __try { throw_11(); }
    __finally { L("fin "); }
  } __except (EXCEPTION_CONTINUE_SEARCH) { L("wrong "); }
}

static void seh_plain_finally(void) {
  __try { L("body "); }
  __finally { L("fin2 "); }
}

static int seh_with_parameters(void) {
  int r = 0;

  __try {
    ULONG_PTR args[2] = {7, 8};
    RaiseException(0xE0001234, 0, 2, args);
  } __except (GetExceptionInformation()->ExceptionRecord->NumberParameters == 2 && GetExceptionInformation()->ExceptionRecord->ExceptionInformation[1] == 8 ? 1 : 0) {
    r = 1;
  }
  return r;
}

static void test_seh(void) {
  int r = seh_except_finally();

  CHECK(r == 1 && g_seh_code == (int)0xE0001234);
  CHECK_LOG("+seh-obj -seh-obj finally except ");

  r = 0;
  try { seh_passes_cxx(); } catch (int v) { r = v; }
  CHECK(r == 11);
  CHECK_LOG("fin ");

  seh_plain_finally();
  CHECK_LOG("body fin2 ");
  CHECK(seh_with_parameters() == 1);
}

static void will_throw(void) noexcept { throw 1; }

int main(int argc, char **argv) {
  printf("cxxtest started, argc=%d\n", argc);
  if (argc > 1 && strcmp(argv[1], "uncaught") == 0) {
    throw 77;
  }
  if (argc > 1 && strcmp(argv[1], "noexcept") == 0) {
    try { will_throw(); } catch (...) { printf("noexcept function's exception was caught: wrong\n"); }
    return 99;
  }
  if (argc > 1 && strcmp(argv[1], "rethrow") == 0) { test_rethrow(); printf("rethrow: %d failures", g_fail); return g_fail; }
  puts("-- basic"); test_basic();
  puts("-- classes"); test_classes();
  puts("-- std"); test_std_exception();
  puts("-- rethrow"); test_rethrow();
  puts("-- construction"); test_construction();
  puts("-- many"); test_many();
  puts("-- new"); test_new();
  puts("-- seh"); test_seh();
  puts("-- threads"); test_threads();
  printf("cxxtest: %d checks, %d failures\n", g_checks, g_fail);
  return g_fail;
}
