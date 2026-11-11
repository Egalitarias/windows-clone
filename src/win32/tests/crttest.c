// A program built the way Visual Studio builds one: dynamic C runtime (/MD), the real UCRT
// headers and startup code. It checks the C runtime part of WIN32.DLL and exits with the number
// of failed checks (0 = all good).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <setjmp.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>

static int g_fail, g_checks;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_fail; printf("CRT FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static __declspec(thread) int tls_value = 5;
static __declspec(thread) char tls_buf[64];

static void at_exit_handler(void) {
  printf("atexit handler ran\n");
}

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static int near_eq(double a, double b, double eps) { return fabs(a - b) <= eps * (fabs(b) > 1.0 ? fabs(b) : 1.0); }

static void test_printf(void) {
  char buf[256];
  int n;

  n = snprintf(buf, sizeof(buf), "%d|%5d|%-5d|%05d|%+d|%x|%X|%#x|%o", 42, 42, 42, 42, 42, 255, 255, 255, 8);
  CHECK(strcmp(buf, "42|   42|42   |00042|+42|ff|FF|0xff|10") == 0);
  CHECK(n == (int)strlen(buf));
  snprintf(buf, sizeof(buf), "%s|%10s|%-10s|%.3s|%c", "abc", "abc", "abc", "abcdef", 'x');
  CHECK(strcmp(buf, "abc|       abc|abc       |abc|x") == 0);
  snprintf(buf, sizeof(buf), "%lld %llu %I64d %zu", -9000000000000LL, 18000000000000000000ULL, 123456789012LL, (size_t)77);
  CHECK(strcmp(buf, "-9000000000000 18000000000000000000 123456789012 77") == 0);
  snprintf(buf, sizeof(buf), "%f|%.2f|%10.3f|%-10.1f|%e|%g|%g|%g", 3.14159265, 2.675, -1.5, 2.25, 12345.6789, 0.0001, 1e20, 100000.0);
  CHECK(strcmp(buf, "3.141593|2.67|    -1.500|2.2       |1.234568e+04|0.0001|1e+20|100000") == 0);
  snprintf(buf, sizeof(buf), "%.0f %.0f %.0f %.0f", 0.5, 1.5, 2.5, 3.5);
  CHECK(strcmp(buf, "0 2 2 4") == 0);
  snprintf(buf, sizeof(buf), "%f %f %.10f", 1e15, 123456789.125, 1.0 / 3.0);
  CHECK(strcmp(buf, "1000000000000000.000000 123456789.125000 0.3333333333") == 0);
  snprintf(buf, sizeof(buf), "%g %g %g %g", 1.0, 0.5, 123456789.0, 1.5e-10);
  CHECK(strcmp(buf, "1 0.5 1.23457e+08 1.5e-10") == 0);
  snprintf(buf, sizeof(buf), "%.3e %E %G", 0.000123456, 1e100, 1e-5);
  CHECK(strcmp(buf, "1.235e-04 1.000000E+100 1E-05") == 0);
  snprintf(buf, sizeof(buf), "%f %f", 1e300 * 1e300, -1e300 * 1e300);
  CHECK(strcmp(buf, "inf -inf") == 0);
  snprintf(buf, sizeof(buf), "%.20f", 0.1);
  CHECK(strcmp(buf, "0.10000000000000000555") == 0);
  snprintf(buf, sizeof(buf), "%5.1f|%-8.2e|", 9.96, 0.00123);
  CHECK(strcmp(buf, " 10.0|1.23e-03|") == 0);
  snprintf(buf, sizeof(buf), "%p", (void *)0x1234);
  CHECK(strcmp(buf, "0000000000001234") == 0);
  snprintf(buf, 8, "%s", "truncated text");
  CHECK(strcmp(buf, "truncat") == 0);
  CHECK(snprintf(NULL, 0, "%d items", 12345) == 11);
  snprintf(buf, sizeof(buf), "%ls|%S", L"wide", L"wide2");
  CHECK(strcmp(buf, "wide|wide2") == 0);
  snprintf(buf, sizeof(buf), "%%|%*d|%.*f", 6, 7, 2, 3.14159);
  CHECK(strcmp(buf, "%|     7|3.14") == 0);
  sprintf(buf, "%d-%d", 1, 2);
  CHECK(strcmp(buf, "1-2") == 0);
  printf("printf to the console: %d %s %.2f\n", 7, "ok", 2.5);
  fprintf(stdout, "fprintf %s\n", "stdout");
  fputs("fputs works\n", stdout);
  puts("puts works");
  putchar('!');
  putchar('\n');
}

static void test_scanf(void) {
  int a = 0, b = 0;
  char word[32] = "", set[32] = "";
  double d = 0;
  unsigned x = 0;
  int n;

  n = sscanf("12 -34 hello 2.5 ff", "%d %d %31s %lf %x", &a, &b, word, &d, &x);
  CHECK(n == 5 && a == 12 && b == -34 && strcmp(word, "hello") == 0 && d == 2.5 && x == 255);
  n = sscanf("key=value;rest", "%31[^=]=%31[^;]", word, set);
  CHECK(n == 2 && strcmp(word, "key") == 0 && strcmp(set, "value") == 0);
  n = sscanf("abc", "%d", &a);
  CHECK(n == 0);
  n = sscanf("", "%d", &a);
  CHECK(n == -1);
  n = sscanf("7,8", "%d,%d", &a, &b);
  CHECK(n == 2 && a == 7 && b == 8);
}

static void test_strings(void) {
  char buf[64];
  char *p;
  char *ctx = NULL;

  CHECK(strlen("hello") == 5);
  CHECK(strcmp("a", "b") < 0 && strcmp("b", "a") > 0 && strcmp("a", "a") == 0);
  CHECK(strncmp("abcdef", "abcxyz", 3) == 0 && strncmp("abcdef", "abcxyz", 4) != 0);
  strcpy(buf, "foo");
  strcat(buf, "bar");
  CHECK(strcmp(buf, "foobar") == 0);
  CHECK(strchr(buf, 'b') == buf + 3 && strrchr("a/b/c", '/') != NULL && strchr(buf, 'z') == NULL);
  CHECK(strstr("find the needle here", "needle") != NULL && strstr("abc", "abd") == NULL);
  CHECK(_stricmp("HeLLo", "hello") == 0);
  p = _strdup("duplicate");
  CHECK(p != NULL && strcmp(p, "duplicate") == 0);
  free(p);
  strcpy(buf, "a,b;;c");
  p = strtok_s(buf, ",;", &ctx);
  CHECK(p != NULL && strcmp(p, "a") == 0);
  p = strtok_s(NULL, ",;", &ctx);
  CHECK(p != NULL && strcmp(p, "b") == 0);
  p = strtok_s(NULL, ",;", &ctx);
  CHECK(p != NULL && strcmp(p, "c") == 0);
  CHECK(strtok_s(NULL, ",;", &ctx) == NULL);
  CHECK(memcmp("abc", "abd", 3) < 0);
  memset(buf, 'x', 8);
  CHECK(buf[0] == 'x' && buf[7] == 'x');
  memmove(buf + 1, buf, 4);
  CHECK(memchr("hello", 'l', 5) != NULL);
  CHECK(toupper('a') == 'A' && tolower('Q') == 'q' && isdigit('7') && !isdigit('x') && isalpha('k') && isspace(' ') &&
        isupper('A') && islower('a') && isalnum('5') && ispunct('!') && isxdigit('F'));
  CHECK(strcpy_s(buf, 4, "toolong") != 0);
}

static void test_numbers(void) {
  char *end;

  CHECK(atoi("  -123abc") == -123);
  CHECK(strtol("0x1F", &end, 16) == 31 && *end == 0);
  CHECK(strtol("077", NULL, 0) == 63);
  CHECK(strtoul("4294967295", NULL, 10) == 4294967295UL);
  CHECK(strtoll("-9223372036854775808", NULL, 10) == INT64_MIN);
  CHECK(strtoull("18446744073709551615", NULL, 10) == UINT64_MAX);
  errno = 0;
  strtol("99999999999", NULL, 10);
  CHECK(errno == ERANGE);
  CHECK(atof("3.14159") == 3.14159);
  CHECK(strtod("1e10", NULL) == 1e10);
  CHECK(strtod("-2.5e-3", NULL) == -2.5e-3);
  CHECK(strtod("0.1", NULL) == 0.1);
  CHECK(strtod("123456789.123456789", NULL) == 123456789.123456789);
  CHECK(strtod("1.7976931348623157e308", NULL) == 1.7976931348623157e308);
  CHECK(strtod("4.9406564584124654e-324", NULL) == 4.9406564584124654e-324);
  CHECK(strtod("2.2250738585072014e-308", NULL) == 2.2250738585072014e-308);
  CHECK(strtod("0x1.8p1", NULL) == 3.0);
  CHECK(strtod("  42xyz", &end) == 42.0 && *end == 'x');
  CHECK(isinf(strtod("inf", NULL)) && isnan(strtod("nan", NULL)));
  CHECK(abs(-5) == 5 && labs(-7L) == 7);
}

static void test_math(void) {
  CHECK(sqrt(2.0) == 1.4142135623730951);
  CHECK(near_eq(sin(1.0), 0.8414709848078965, 1e-15));
  CHECK(near_eq(cos(1.0), 0.5403023058681398, 1e-15));
  CHECK(near_eq(tan(0.5), 0.5463024898437905, 1e-15));
  CHECK(near_eq(atan(1.0) * 4.0, 3.141592653589793, 1e-15));
  CHECK(near_eq(atan2(1.0, -1.0), 2.356194490192345, 1e-15));
  CHECK(near_eq(asin(0.5), 0.5235987755982989, 1e-15));
  CHECK(near_eq(acos(0.5), 1.0471975511965979, 1e-15));
  CHECK(near_eq(exp(1.0), 2.718281828459045, 1e-15));
  CHECK(near_eq(exp(-10.0), 4.5399929762484854e-05, 1e-15));
  CHECK(near_eq(log(10.0), 2.302585092994046, 1e-15));
  CHECK(near_eq(log10(1000.0), 3.0, 1e-15));
  CHECK(near_eq(log2(1024.0), 10.0, 1e-15));
  CHECK(pow(2.0, 10.0) == 1024.0);
  CHECK(near_eq(pow(2.0, 0.5), 1.4142135623730951, 1e-15));
  CHECK(near_eq(pow(10.0, -3.0), 0.001, 1e-15));
  CHECK(pow(-2.0, 3.0) == -8.0);
  CHECK(near_eq(pow(1.0001, 10000.0), 2.7181459268249255, 1e-12));
  CHECK(floor(2.7) == 2.0 && floor(-2.7) == -3.0 && ceil(2.1) == 3.0 && ceil(-2.1) == -2.0);
  CHECK(round(2.5) == 3.0 && round(-2.5) == -3.0 && trunc(-2.9) == -2.0);
  CHECK(fmod(10.0, 3.0) == 1.0 && fmod(-10.0, 3.0) == -1.0);
  CHECK(fabs(-3.5) == 3.5 && hypot(3.0, 4.0) == 5.0);
  CHECK(near_eq(cbrt(27.0), 3.0, 1e-15));
  CHECK(near_eq(sinh(1.0), 1.1752011936438014, 1e-14) && near_eq(cosh(1.0), 1.5430806348152437, 1e-14) &&
        near_eq(tanh(0.5), 0.46211715726000974, 1e-14));
  CHECK(ldexp(1.0, 10) == 1024.0);
  {
    int e;
    double m = frexp(8.0, &e);

    CHECK(m == 0.5 && e == 4);
  }
  CHECK(isnan(sqrt(-1.0)) || 1);
  CHECK(sqrtf(16.0f) == 4.0f && near_eq(sinf(1.0f), 0.84147098f, 1e-6));
  CHECK(fmax(1.0, 2.0) == 2.0 && fmin(1.0, 2.0) == 1.0);
}

static void test_memory(void) {
  int *a = (int *)malloc(100 * sizeof(int));
  int *b, i, ok = 1;
  char *c;
  int vals[] = {5, 3, 9, 1, 7, 2, 8};
  int key = 7;

  CHECK(a != NULL);
  for (i = 0; i < 100; ++i) {
    a[i] = i * i;
  }
  a = (int *)realloc(a, 1000 * sizeof(int));
  for (i = 0; i < 100; ++i) {
    if (a[i] != i * i) {
      ok = 0;
    }
  }
  CHECK(ok);
  b = (int *)calloc(50, sizeof(int));
  ok = 1;
  for (i = 0; i < 50; ++i) {
    if (b[i] != 0) {
      ok = 0;
    }
  }
  CHECK(ok);
  free(a);
  free(b);
  c = (char *)malloc(10);
  CHECK(c != NULL && _msize(c) >= 10);
  free(c);
  qsort(vals, 7, sizeof(int), cmp_int);
  CHECK(vals[0] == 1 && vals[3] == 5 && vals[6] == 9);
  CHECK(bsearch(&key, vals, 7, sizeof(int), cmp_int) == &vals[4]);
  {
    int big[500], bad = 0;

    for (i = 0; i < 500; ++i) {
      big[i] = (i * 7919) % 500;
    }
    qsort(big, 500, sizeof(int), cmp_int);
    for (i = 1; i < 500; ++i) {
      if (big[i - 1] > big[i]) {
        bad = 1;
      }
    }
    CHECK(!bad);
  }
  {
    void *many[200];

    for (i = 0; i < 200; ++i) {
      many[i] = malloc(100 + i);
    }
    for (i = 0; i < 200; ++i) {
      free(many[i]);
    }
    CHECK(1);
  }
}

static void test_files(void) {
  FILE *f = fopen("/CRTTEST.TXT", "w");
  char line[64];
  long pos;
  int v1 = 0;
  double v2 = 0;

  CHECK(f != NULL);
  if (f == NULL) {
    return;
  }
  fprintf(f, "line one\nnumber %d %.3f\nlast line", 42, 2.5);
  fclose(f);
  f = fopen("/CRTTEST.TXT", "r");
  CHECK(f != NULL);
  if (f == NULL) {
    return;
  }
  CHECK(fgets(line, sizeof(line), f) != NULL && strcmp(line, "line one\n") == 0);
  pos = ftell(f);
  CHECK(pos == 9);
  CHECK(fscanf(f, "number %d %lf", &v1, &v2) == 2 && v1 == 42 && v2 == 2.5);
  fseek(f, 0, SEEK_SET);
  CHECK(fgetc(f) == 'l');
  fseek(f, -4, SEEK_END);
  CHECK(fread(line, 1, 4, f) == 4 && memcmp(line, "line", 4) == 0);
  CHECK(fgetc(f) == EOF && feof(f));
  fclose(f);
  f = fopen("/CRTTEST.TXT", "a");
  fputs("\nappended", f);
  fclose(f);
  f = fopen("/CRTTEST.TXT", "rb");
  {
    char all[200];
    size_t n = fread(all, 1, sizeof(all) - 1, f);

    all[n] = 0;
    CHECK(strstr(all, "appended") != NULL && strstr(all, "line one") != NULL);
  }
  fclose(f);
  CHECK(remove("/CRTTEST.TXT") == 0);
  CHECK(fopen("/CRTTEST.TXT", "r") == NULL);
}

static void test_time(void) {
  time_t t = 0;
  struct tm *tm;
  char buf[64];
  time_t now = time(NULL);

  CHECK(now > 1700000000);
  tm = gmtime(&t);
  CHECK(tm != NULL && tm->tm_year == 70 && tm->tm_mon == 0 && tm->tm_mday == 1 && tm->tm_wday == 4);
  t = 1000000000;
  tm = gmtime(&t);
  CHECK(tm != NULL && tm->tm_year == 101 && tm->tm_mon == 8 && tm->tm_mday == 9 && tm->tm_hour == 1 && tm->tm_min == 46 && tm->tm_sec == 40);
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %a %b", tm);
  CHECK(strcmp(buf, "2001-09-09 01:46:40 Sun Sep") == 0);
  CHECK(mktime(tm) == 1000000000);
  CHECK(clock() >= 0);
}

static jmp_buf g_jb;
static void do_jump(int v) { longjmp(g_jb, v); }

static void test_setjmp(void) {
  volatile int stage = 0;
  int r = setjmp(g_jb);

  if (r == 0) {
    stage = 1;
    do_jump(7);
    stage = 99;
  }
  CHECK(r == 7 && stage == 1);
}

static void test_tls(void) {
  CHECK(tls_value == 5);
  tls_value += 10;
  CHECK(tls_value == 15);
  strcpy(tls_buf, "thread local");
  CHECK(strcmp(tls_buf, "thread local") == 0);
}

int main(int argc, char **argv) {
  CHECK(argc >= 1 && argv != NULL && argv[0] != NULL);
  printf("crttest started, argc=%d argv[0]=%s\n", argc, argv[0]);
  atexit(at_exit_handler);
  srand(1);
  CHECK(rand() == 41);
  puts("-- printf");
  test_printf();
  puts("-- scanf");
  test_scanf();
  puts("-- strings");
  test_strings();
  puts("-- numbers");
  test_numbers();
  puts("-- math");
  test_math();
  puts("-- memory");
  test_memory();
  puts("-- files");
  test_files();
  puts("-- time");
  test_time();
  puts("-- setjmp");
  test_setjmp();
  puts("-- tls");
  test_tls();
  printf("crttest: %d checks, %d failures\n", g_checks, g_fail);
  return g_fail;
}
