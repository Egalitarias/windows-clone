#include "shell.h"
#include "apps.h"
#include "console.h"
#include "desktop.h"
#include "fs.h"
#include "pmm.h"
#include "process.h"
#include "rtc.h"
#include "string.h"
#include "timer.h"

// The command interpreter behind the Console window. Output goes through
// console_printf, so it lands in the same log the window shows (and on the serial
// port). Paths use '/', are looked up on the "current volume" (the default one until
// `use` picks another) and are case-insensitive like FAT itself.

#define PATH_MAX_LEN 128
#define CAT_MAX      4096

static char g_cwd[PATH_MAX_LEN] = "/";      // absolute path on the current volume
static int g_vol = -1;                      // current volume index, -1 = the default one

// ---- helpers ---------------------------------------------------------------

static int is_space(char c) { return c == ' ' || c == '\t'; }

static fat_volume_t *current_volume(void) {
  if (g_vol >= 0 && g_vol < fs_volume_count()) {
    return fs_volume(g_vol);
  }
  g_vol = -1;
  return fs_default_volume();
}

// Build the absolute, normalised form of `in` (relative to the cwd) into `out`:
// no "." or ".." parts, no repeated or trailing slashes. Returns 0 if it is too long.
static int normalize(const char *in, char *out) {
  char tmp[2 * PATH_MAX_LEN];
  int len = 0, n = 0, i;

  if (*in != '/') {
    len = (int)str_copy(tmp, g_cwd, sizeof(tmp));
    if (len > 1 && len < (int)sizeof(tmp) - 1) {
      tmp[len++] = '/';
    }
  }
  while (*in && len < (int)sizeof(tmp) - 1) {
    tmp[len++] = *in++;
  }
  tmp[len] = 0;

  out[n++] = '/';
  i = 0;
  while (tmp[i]) {
    char part[FAT_NAME_MAX];
    int pl = 0;

    while (tmp[i] == '/') {
      i++;
    }
    while (tmp[i] && tmp[i] != '/') {
      if (pl + 1 >= (int)sizeof(part)) {
        return 0;
      }
      part[pl++] = tmp[i++];
    }
    part[pl] = 0;
    if (pl == 0 || (pl == 1 && part[0] == '.')) {
      continue;
    }
    if (pl == 2 && part[0] == '.' && part[1] == '.') {
      while (n > 1 && out[n - 1] != '/') {      // drop the last component
        n--;
      }
      if (n > 1) {
        n--;                                    // and its slash
      }
      continue;
    }
    if (n > 1) {
      if (n + 1 >= PATH_MAX_LEN) {
        return 0;
      }
      out[n++] = '/';
    }
    if (n + pl >= PATH_MAX_LEN) {
      return 0;
    }
    memcpy(out + n, part, (size_t)pl);
    n += pl;
  }
  out[n] = 0;
  return 1;
}

// Resolve an absolute normalised path on the current volume. For "/" *is_root is set
// and ent is not filled. Returns 1 when it exists.
static int resolve(const char *abs, fat_dirent_t *ent, int *is_root) {
  fat_volume_t *v = current_volume();
  uint32_t dir;
  int i = 1;

  *is_root = 0;
  if (v == 0) {
    return 0;
  }
  dir = v->root_cluster;
  if (abs[1] == 0) {
    *is_root = 1;
    return 1;
  }
  for (;;) {
    char part[FAT_NAME_MAX];
    int pl = 0;

    while (abs[i] && abs[i] != '/') {
      part[pl++] = abs[i++];
    }
    part[pl] = 0;
    if (!fat_lookup(v, dir, part, ent)) {
      return 0;
    }
    if (abs[i] == 0) {
      return 1;
    }
    i++;
    if (!(ent->attr & FAT_ATTR_DIR)) {
      return 0;
    }
    dir = ent->first_cluster;
  }
}

// Split the line into words in place. Returns the word count.
static int split_words(char *line, char **word, int max) {
  int n = 0;

  while (*line && n < max) {
    while (is_space(*line)) {
      *line++ = 0;
    }
    if (*line == 0) {
      break;
    }
    word[n++] = line;
    while (*line && !is_space(*line)) {
      line++;
    }
  }
  return n;
}

// ---- commands --------------------------------------------------------------

static void cmd_help(void) {
  console_printf("Commands:\r\n");
  console_printf("  help                 this list\r\n");
  console_printf("  clear                empty the console\r\n");
  console_printf("  echo TEXT            print TEXT\r\n");
  console_printf("  ver                  version\r\n");
  console_printf("  uptime               time since boot\r\n");
  console_printf("  time                 local date and time\r\n");
  console_printf("  mem                  free memory\r\n");
  console_printf("  vol                  list volumes\r\n");
  console_printf("  use N                switch to volume N\r\n");
  console_printf("  pwd, cd DIR, ls [DIR]   look around the current volume\r\n");
  console_printf("  cat FILE             show a text file\r\n");
  console_printf("  run PROG [ARG]       start a program (PROG alone means /APPS/PROG.BIN)\r\n");
  console_printf("  notify [TEXT]        show a notification\r\n");
  console_printf("  shutdown             power off\r\n");
}

static void cmd_uptime(void) {
  uint64_t s = timer_get_ms() / 1000ULL;

  console_printf("up %llu:%d%d:%d%d\r\n", s / 3600ULL, (int)(s / 60ULL % 60ULL) / 10,
                 (int)(s / 60ULL % 60ULL) % 10, (int)(s % 60ULL) / 10, (int)(s % 60ULL) % 10);
}

static void cmd_time(void) {
  int y, mo, d, h, m;

  if (!rtc_read_local(desktop_tz_offset_min(), &y, &mo, &d, &h, &m)) {
    console_printf("the clock cannot be read\r\n");
    return;
  }
  console_printf("%d-%d%d-%d%d %d%d:%d%d\r\n", y, mo / 10, mo % 10, d / 10, d % 10,
                 h / 10, h % 10, m / 10, m % 10);
}

static void cmd_mem(void) {
  uint64_t pages = pmm_available_pages();

  console_printf("free memory: %llu MiB (%llu pages)\r\n", pages * PAGE_SIZE / (1024ULL * 1024ULL), pages);
}

static void cmd_vol(void) {
  int i;

  if (fs_volume_count() == 0) {
    console_printf("no volumes\r\n");
    return;
  }
  for (i = 0; i < fs_volume_count(); i++) {
    console_printf("%s %d  %s\r\n", fs_volume(i) == current_volume() ? "*" : " ", i, fs_volume_name(i));
  }
}

static void cmd_use(int argc, char **argv) {
  int n = 0, i;

  if (argc < 2 || argv[1][0] < '0' || argv[1][0] > '9') {
    console_printf("usage: use N   (see `vol`)\r\n");
    return;
  }
  for (i = 0; argv[1][i] >= '0' && argv[1][i] <= '9' && i < 4; i++) {
    n = n * 10 + (argv[1][i] - '0');
  }
  if (n >= fs_volume_count()) {
    console_printf("no volume %d\r\n", n);
    return;
  }
  g_vol = n;
  g_cwd[0] = '/';
  g_cwd[1] = 0;
  console_printf("volume %d: %s\r\n", n, fs_volume_name(n));
}

static void cmd_cd(int argc, char **argv) {
  char abs[PATH_MAX_LEN];
  fat_dirent_t ent;
  int root;

  if (argc < 2) {
    console_printf("%s\r\n", g_cwd);
    return;
  }
  if (!normalize(argv[1], abs) || !resolve(abs, &ent, &root)) {
    console_printf("cd: no such folder: %s\r\n", argv[1]);
    return;
  }
  if (!root && !(ent.attr & FAT_ATTR_DIR)) {
    console_printf("cd: not a folder: %s\r\n", argv[1]);
    return;
  }
  str_copy(g_cwd, abs, sizeof(g_cwd));
}

static void cmd_ls(int argc, char **argv) {
  char abs[PATH_MAX_LEN];
  fat_dirent_t ent, e;
  fat_iter_t it;
  fat_volume_t *v = current_volume();
  uint32_t dir;
  int root, count = 0;

  if (v == 0 || !normalize(argc > 1 ? argv[1] : ".", abs) || !resolve(abs, &ent, &root)) {
    console_printf("ls: no such folder: %s\r\n", argc > 1 ? argv[1] : ".");
    return;
  }
  if (root) {
    dir = v->root_cluster;
  } else if (ent.attr & FAT_ATTR_DIR) {
    dir = ent.first_cluster;
  } else {
    console_printf("%s  %u bytes\r\n", ent.name, ent.size);
    return;
  }
  fat_iter_open(v, dir, &it);
  while (fat_iter_next(&it, &e)) {
    if (e.attr & FAT_ATTR_DIR) {
      console_printf("  <DIR>        %s\r\n", e.name);
    } else {
      console_printf("  %u  %s\r\n", e.size, e.name);
    }
    count++;
  }
  console_printf("%d item%s\r\n", count, count == 1 ? "" : "s");
}

static void cmd_cat(int argc, char **argv) {
  char abs[PATH_MAX_LEN];
  static char buf[CAT_MAX + 1];
  fat_dirent_t ent;
  uint32_t got = 0, i;
  int root;

  if (argc < 2) {
    console_printf("usage: cat FILE\r\n");
    return;
  }
  if (!normalize(argv[1], abs) || !resolve(abs, &ent, &root) || root || (ent.attr & FAT_ATTR_DIR)) {
    console_printf("cat: no such file: %s\r\n", argv[1]);
    return;
  }
  if (!fat_read_file(current_volume(), &ent, buf, CAT_MAX, &got)) {
    console_printf("cat: cannot read %s\r\n", argv[1]);
    return;
  }
  for (i = 0; i < got; i++) {            // text only: other bytes become '.'
    char c = buf[i];
    if (c == '\r') {
      continue;
    }
    buf[i] = (c == '\n' || (c >= 32 && c < 127)) ? c : '.';
  }
  buf[got] = 0;
  console_printf("%s", buf);
  if (got == 0 || buf[got - 1] != '\n') {
    console_printf("\r\n");
  }
  if (ent.size > got) {
    console_printf("... (%u more bytes not shown)\r\n", ent.size - got);
  }
}

static void cmd_run(int argc, char **argv) {
  char path[PATH_MAX_LEN];
  int has_slash = 0, has_dot = 0, i;

  if (argc < 2) {
    console_printf("usage: run PROG\r\n");
    return;
  }
  for (i = 0; argv[1][i]; i++) {
    has_slash |= argv[1][i] == '/';
    has_dot |= argv[1][i] == '.';
  }
  if (has_slash) {
    if (!normalize(argv[1], path)) {
      console_printf("run: path too long\r\n");
      return;
    }
  } else {
    int n = (int)str_copy(path, "/APPS/", sizeof(path));
    n += (int)str_copy(path + n, argv[1], sizeof(path) - (size_t)n);
    if (!has_dot) {
      str_copy(path + n, ".BIN", sizeof(path) - (size_t)n);
    }
  }
  if (!process_program_exists(path)) {
    console_printf("run: no such program: %s\r\n", path);
    return;
  }
  console_printf("starting %s\r\n", path);
  app_programs_run(path);
}

// ---- entry point -----------------------------------------------------------

void shell_exec(const char *input) {
  char line[128];
  char *argv[8];
  int argc;
  char *p;

  str_copy(line, input, sizeof(line));
  // `echo` keeps the rest of the line as it was typed.
  p = line;
  while (is_space(*p)) {
    p++;
  }
  if (p[0] == 'e' && p[1] == 'c' && p[2] == 'h' && p[3] == 'o' && (p[4] == 0 || is_space(p[4]))) {
    p += 4;
    while (is_space(*p)) {
      p++;
    }
    console_printf("%s\r\n", p);
    return;
  }
  argc = split_words(line, argv, 8);
  if (argc == 0) {
    return;
  }
  if (str_eq_nocase(argv[0], "help") || str_eq_nocase(argv[0], "?")) {
    cmd_help();
  } else if (str_eq_nocase(argv[0], "clear") || str_eq_nocase(argv[0], "cls")) {
    console_log_clear();
  } else if (str_eq_nocase(argv[0], "ver")) {
    console_printf("WinClone kernel, built %s %s\r\n", __DATE__, __TIME__);
  } else if (str_eq_nocase(argv[0], "uptime")) {
    cmd_uptime();
  } else if (str_eq_nocase(argv[0], "time") || str_eq_nocase(argv[0], "date")) {
    cmd_time();
  } else if (str_eq_nocase(argv[0], "mem")) {
    cmd_mem();
  } else if (str_eq_nocase(argv[0], "vol")) {
    cmd_vol();
  } else if (str_eq_nocase(argv[0], "use")) {
    cmd_use(argc, argv);
  } else if (str_eq_nocase(argv[0], "pwd")) {
    console_printf("%s\r\n", g_cwd);
  } else if (str_eq_nocase(argv[0], "cd")) {
    cmd_cd(argc, argv);
  } else if (str_eq_nocase(argv[0], "ls") || str_eq_nocase(argv[0], "dir")) {
    cmd_ls(argc, argv);
  } else if (str_eq_nocase(argv[0], "cat") || str_eq_nocase(argv[0], "type")) {
    cmd_cat(argc, argv);
  } else if (str_eq_nocase(argv[0], "run")) {
    cmd_run(argc, argv);
  } else if (str_eq_nocase(argv[0], "notify")) {
    const char *t = input;
    while (is_space(*t)) { t++; }
    t += 6;                                   // past "notify"
    while (is_space(*t)) { t++; }
    desktop_notify(*t ? t : "Hello from the Console");
  } else if (str_eq_nocase(argv[0], "shutdown")) {
    desktop_shut_down();
  } else {
    console_printf("unknown command: %s (try `help`)\r\n", argv[0]);
  }
}

const char *shell_cwd(void) {
  return g_cwd;
}
