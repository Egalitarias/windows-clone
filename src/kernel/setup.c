#include "apps.h"
#include "block.h"
#include "desktop.h"
#include "disk.h"
#include "fb.h"
#include "format.h"
#include "fs.h"
#include "string.h"
#include "wm.h"

// Setup: installs WinClone onto a disk. It erases the chosen disk, writes a GPT
// with an EFI System Partition and a data partition, formats both as FAT32,
// copies \EFI\BOOT\BOOTX64.EFI and KERNEL.BIN, and the programs in \APPS, from the boot
// medium into the ESP and reads them back to verify. The disk the system booted from is refused.

#define MAX_LOG       8
#define LOG_LEN       80
#define ROW_Y0        64
#define ROW_H         22
#define BTN_Y         214
#define BTN_MAIN_X    16
#define BTN_MAIN_W    200
#define BTN_CANCEL_X  224
#define BTN_CANCEL_W  100
#define BTN_H         26
#define LOG_Y0        252
#define ESP_MIB       100

#define MAX_BOOT_FILE   (64 * 1024)
#define MAX_KERNEL_FILE (512 * 1024)

typedef enum {
  STATE_SELECT = 0,
  STATE_CONFIRM,
  STATE_RUNNING,
  STATE_DONE
} setup_state_t;

static wm_window_t *g_win = 0;
static setup_state_t g_state = STATE_SELECT;
static int g_selected = -1;                 // disk index
static int g_boot_disk = -1;                // disk holding the volume we boot from
static char g_log[MAX_LOG][LOG_LEN];
static int g_log_count = 0;
static int g_progress = -1;

static uint8_t g_boot_file[MAX_BOOT_FILE];
static uint8_t g_kernel_file[MAX_KERNEL_FILE];
static uint8_t g_verify[MAX_KERNEL_FILE];
static fat_volume_t g_target;               // the freshly formatted ESP
static fat_volume_t *g_src_vol;             // the volume the boot files were read from
static fat_iter_t g_apps_iter;

// ---- helpers ---------------------------------------------------------------

static void log_line(const char *a, const char *b) {
  int n;
  if (g_log_count == MAX_LOG) {
    int i;
    for (i = 1; i < MAX_LOG; i++) {
      memcpy(g_log[i - 1], g_log[i], LOG_LEN);
    }
    g_log_count--;
  }
  n = (int)str_copy(g_log[g_log_count], a, LOG_LEN);
  if (b) {
    str_copy(g_log[g_log_count] + n, b, (size_t)(LOG_LEN - n));
  }
  g_log_count++;
  if (g_win) {
    wm_invalidate_window(g_win);
    desktop_pump();
  }
}

static void size_text(uint64_t sectors, char *out) {
  uint64_t mib = sectors / 2048u;
  int n;
  if (mib >= 1024) {
    uint64_t tenths = (mib * 10u / 1024u) % 10u;
    n = u32_to_str((uint32_t)(mib / 1024u), out);
    out[n++] = '.';
    out[n++] = (char)('0' + tenths);
    str_copy(out + n, " GB", 4);
  } else {
    n = u32_to_str((uint32_t)mib, out);
    str_copy(out + n, " MB", 4);
  }
}

static int disk_selectable(int i) {
  const disk_info_t *d = disk_get(i);
  return d && i != g_boot_disk && d->sectors >= 8192ull * 40ull;     // >= 160 MB
}

// ---- boot files ------------------------------------------------------------

// Locate \EFI\BOOT\<name> on a mounted volume. Returns 1 and fills the entry.
static int find_boot_file(fat_volume_t *v, const char *name, fat_dirent_t *out) {
  fat_dirent_t efi, boot;
  if (!fat_lookup(v, v->root_cluster, "EFI", &efi) || !(efi.attr & FAT_ATTR_DIR)) {
    return 0;
  }
  if (!fat_lookup(v, efi.first_cluster, "BOOT", &boot) || !(boot.attr & FAT_ATTR_DIR)) {
    return 0;
  }
  return fat_lookup(v, boot.first_cluster, name, out);
}

// Find the volume we booted from and remember which disk it lives on.
static void detect_boot_disk(void) {
  int i;
  fat_dirent_t e1, e2;
  g_boot_disk = -1;
  for (i = 0; i < fs_volume_count(); i++) {
    fat_volume_t *v = fs_volume(i);
    if (find_boot_file(v, "BOOTX64.EFI", &e1) && find_boot_file(v, "KERNEL.BIN", &e2)) {
      const block_dev_t *b = block_get(v->dev);
      if (b) {
        g_boot_disk = b->disk;
      }
      return;
    }
  }
}

static int load_sources(uint32_t *boot_len, uint32_t *kernel_len) {
  int i;
  fat_dirent_t eb, ek;
  for (i = 0; i < fs_volume_count(); i++) {
    fat_volume_t *v = fs_volume(i);
    if (find_boot_file(v, "BOOTX64.EFI", &eb) && find_boot_file(v, "KERNEL.BIN", &ek)) {
      if (eb.size > MAX_BOOT_FILE || ek.size > MAX_KERNEL_FILE) {
        log_line("Boot files are larger than Setup can handle", 0);
        return 0;
      }
      if (!fat_read_file(v, &eb, g_boot_file, MAX_BOOT_FILE, boot_len) ||
          !fat_read_file(v, &ek, g_kernel_file, MAX_KERNEL_FILE, kernel_len)) {
        log_line("Could not read the boot files", 0);
        return 0;
      }
      g_src_vol = v;
      return 1;
    }
  }
  log_line("Boot files (EFI/BOOT/BOOTX64.EFI, KERNEL.BIN) not found", 0);
  return 0;
}

// ---- installation ----------------------------------------------------------

static void format_progress(int percent) {
  if (percent / 10 != g_progress / 10 || percent == 100) {
    char num[8];
    int n = u32_to_str((uint32_t)percent, num);
    num[n++] = '%';
    num[n] = 0;
    g_progress = percent;
    log_line("  formatting ", num);
  }
}

static int copy_and_verify(fat_volume_t *v, uint32_t dir, const char *name,
                           const uint8_t *src, uint32_t len) {
  fat_dirent_t e;
  uint32_t got = 0;
  if (!fat_write_file(v, dir, name, src, len)) {
    return 0;
  }
  if (!fat_lookup(v, dir, name, &e) || e.size != len ||
      !fat_read_file(v, &e, g_verify, MAX_KERNEL_FILE, &got) || got != len ||
      memcmp(g_verify, src, len) != 0) {
    return 0;
  }
  return 1;
}

// Copy every file of \APPS on the boot medium to \APPS on the new ESP (Notes, Files, Paint
// and the other programs the kernel starts from there). A medium without \APPS is not
// an error, but the installed system will then have no programs. Uses g_kernel_file as
// the buffer, so call it after the kernel has been copied.
static int copy_apps(void) {
  fat_dirent_t apps, e;
  uint32_t dir = 0, len = 0;
  int count = 0;
  char num[12];

  if (g_src_vol == 0 || !fat_lookup(g_src_vol, g_src_vol->root_cluster, "APPS", &apps) ||
      !(apps.attr & FAT_ATTR_DIR)) {
    log_line("  no \\APPS folder on the medium: the programs are not installed", 0);
    return 1;
  }
  if (!fat_mkdir(&g_target, g_target.root_cluster, "APPS", &dir)) {
    return 0;
  }
  fat_iter_open(g_src_vol, apps.first_cluster, &g_apps_iter);
  while (fat_iter_next(&g_apps_iter, &e)) {
    if ((e.attr & FAT_ATTR_DIR) != 0 || e.size == 0) {
      continue;
    }
    if (e.size > MAX_KERNEL_FILE) {
      log_line("  skipped (too large): ", e.name);
      continue;
    }
    if (!fat_read_file(g_src_vol, &e, g_kernel_file, MAX_KERNEL_FILE, &len) ||
        !copy_and_verify(&g_target, dir, e.name, g_kernel_file, len)) {
      log_line("FAILED: copying ", e.name);
      return 0;
    }
    ++count;
  }
  u32_to_str((uint32_t)count, num);
  log_line("  programs copied and verified: ", num);
  return 1;
}

static int find_block_at(int disk, uint64_t start) {
  int i;
  for (i = 0; i < block_count(); i++) {
    const block_dev_t *b = block_get(i);
    if (b->disk == disk && b->start == start) {
      return i;
    }
  }
  return -1;
}

static void run_install(int disk) {
  const disk_info_t *d = disk_get(disk);
  uint64_t esp_start, esp_sectors, data_start, data_sectors;
  uint32_t boot_len = 0, kernel_len = 0;
  int dev_esp, dev_data;
  uint32_t efi_dir = 0, boot_dir = 0;
  fat_dirent_t tmp;
  char size[16];

  g_state = STATE_RUNNING;
  g_log_count = 0;
  size_text(d->sectors, size);
  log_line("Installing to ", d->name);
  log_line("  disk size ", size);

  if (!load_sources(&boot_len, &kernel_len)) {
    g_state = STATE_DONE;
    return;
  }
  log_line("Loaded boot files from the installation medium", 0);

  if (!disk_wipe_metadata(disk)) {
    log_line("FAILED: could not clear the disk", 0);
    g_state = STATE_DONE;
    return;
  }
  if (!gpt_layout_install(disk, ESP_MIB, &esp_start, &esp_sectors, &data_start, &data_sectors)) {
    log_line("FAILED: could not write the partition table (disk too small?)", 0);
    g_state = STATE_DONE;
    return;
  }
  log_line("Wrote GPT: EFI System Partition + data partition", 0);

  block_rescan();
  dev_esp = find_block_at(disk, esp_start);
  dev_data = find_block_at(disk, data_start);
  if (dev_esp < 0 || dev_data < 0) {
    log_line("FAILED: new partitions were not detected", 0);
    g_state = STATE_DONE;
    return;
  }

  log_line("Formatting EFI System Partition (FAT32)", 0);
  g_progress = -1;
  if (!fat32_format(dev_esp, "ESP", format_progress)) {
    log_line("FAILED: could not format the ESP", 0);
    g_state = STATE_DONE;
    return;
  }
  log_line("Formatting system partition (FAT32)", 0);
  g_progress = -1;
  if (!fat32_format(dev_data, "WINCLONE", format_progress)) {
    log_line("FAILED: could not format the system partition", 0);
    g_state = STATE_DONE;
    return;
  }

  if (!fat_mount(dev_esp, &g_target)) {
    log_line("FAILED: the new ESP does not mount", 0);
    g_state = STATE_DONE;
    return;
  }
  log_line("Copying boot files to \\EFI\\BOOT", 0);
  if (!fat_mkdir(&g_target, g_target.root_cluster, "EFI", &efi_dir) ||
      !fat_mkdir(&g_target, efi_dir, "BOOT", &boot_dir) ||
      !fat_lookup(&g_target, efi_dir, "BOOT", &tmp)) {
    log_line("FAILED: could not create \\EFI\\BOOT", 0);
    g_state = STATE_DONE;
    return;
  }
  if (!copy_and_verify(&g_target, boot_dir, "BOOTX64.EFI", g_boot_file, boot_len)) {
    log_line("FAILED: BOOTX64.EFI copy did not verify", 0);
    g_state = STATE_DONE;
    return;
  }
  log_line("  BOOTX64.EFI copied and verified", 0);
  if (!copy_and_verify(&g_target, boot_dir, "KERNEL.BIN", g_kernel_file, kernel_len)) {
    log_line("FAILED: KERNEL.BIN copy did not verify", 0);
    g_state = STATE_DONE;
    return;
  }
  log_line("  KERNEL.BIN copied and verified", 0);
  log_line("Copying programs to \\APPS", 0);
  if (!copy_apps()) {
    log_line("FAILED: could not copy the programs", 0);
    g_state = STATE_DONE;
    return;
  }

  g_src_vol = 0;
  fs_rescan();
  detect_boot_disk();
  log_line("Installation complete.", 0);
  log_line("Shut down, remove the installation medium and boot from this disk.", 0);
  g_state = STATE_DONE;
}

// ---- painting --------------------------------------------------------------

static void draw_button(int x, int y, int w, const char *label, int enabled, uint32_t color) {
  fb_fill_rect(x, y, w, BTN_H, enabled ? color : FB_RGB(215, 217, 226));
  fb_draw_rect(x, y, w, BTN_H, FB_RGB(90, 95, 120));
  fb_draw_text(x + (w - fb_text_width(label, 1)) / 2, y + 9, label,
               enabled ? FB_RGB(255, 255, 255) : FB_RGB(140, 142, 155), 1);
}

static void setup_paint(wm_window_t *win, int cx, int cy, int cw, int ch) {
  int i;

  (void)win;
  (void)ch;
  fb_draw_text(cx + 16, cy + 12, "WinClone Setup", FB_RGB(40, 90, 190), 2);
  fb_draw_text(cx + 16, cy + 40, "Choose the disk to install onto. Everything on it is erased.",
               FB_RGB(60, 65, 85), 1);

  for (i = 0; i < disk_count(); i++) {
    const disk_info_t *d = disk_get(i);
    int y = cy + ROW_Y0 + i * ROW_H;
    int ok = disk_selectable(i);
    char size[16], line[96];
    int n;

    size_text(d->sectors, size);
    if (i == g_selected) {
      fb_fill_rect(cx + 12, y, cw - 24, ROW_H - 2, FB_RGB(190, 214, 255));
    } else {
      fb_draw_rect(cx + 12, y, cw - 24, ROW_H - 2, FB_RGB(200, 203, 214));
    }
    n = (int)str_copy(line, d->name, sizeof(line));
    while (n < 8) line[n++] = ' ';
    line[n] = 0;
    n += (int)str_copy(line + n, d->model[0] ? d->model : "(unknown)", 28);
    while (n < 38) line[n++] = ' ';
    line[n] = 0;
    str_copy(line + n, size, sizeof(line) - (size_t)n);
    fb_draw_text(cx + 20, y + 7, line, ok ? FB_RGB(25, 25, 35) : FB_RGB(140, 142, 155), 1);
    if (i == g_boot_disk) {
      fb_draw_text(cx + cw - 12 - fb_text_width("boot disk", 1) - 8, y + 7, "boot disk", FB_RGB(150, 40, 40), 1);
    } else if (!ok) {
      fb_draw_text(cx + cw - 12 - fb_text_width("too small", 1) - 8, y + 7, "too small", FB_RGB(150, 40, 40), 1);
    }
  }
  if (disk_count() == 0) {
    fb_draw_text(cx + 20, cy + ROW_Y0 + 6, "No disks found.", FB_RGB(150, 40, 40), 1);
  }

  if (g_state == STATE_SELECT) {
    draw_button(cx + BTN_MAIN_X, cy + BTN_Y, BTN_MAIN_W, "Install to selected disk",
                g_selected >= 0, FB_RGB(40, 110, 220));
  } else if (g_state == STATE_CONFIRM) {
    char text[80];
    int n = (int)str_copy(text, "ERASE ", sizeof(text));
    str_copy(text + n, disk_get(g_selected)->name, sizeof(text) - (size_t)n);
    fb_draw_text(cx + 16, cy + 190, text, FB_RGB(190, 30, 30), 2);
    draw_button(cx + BTN_MAIN_X, cy + BTN_Y, BTN_MAIN_W, "Erase and install", 1, FB_RGB(190, 40, 40));
    draw_button(cx + BTN_CANCEL_X, cy + BTN_Y, BTN_CANCEL_W, "Cancel", 1, FB_RGB(110, 115, 140));
  } else if (g_state == STATE_RUNNING) {
    fb_draw_text(cx + 16, cy + 222, "Installing... do not turn off the computer.", FB_RGB(190, 100, 20), 1);
  } else {
    draw_button(cx + BTN_MAIN_X, cy + BTN_Y, BTN_MAIN_W, "Start over", 1, FB_RGB(110, 115, 140));
  }

  fb_fill_rect(cx + 12, cy + LOG_Y0 - 4, cw - 24, MAX_LOG * 12 + 8, FB_RGB(248, 248, 252));
  fb_draw_rect(cx + 12, cy + LOG_Y0 - 4, cw - 24, MAX_LOG * 12 + 8, FB_RGB(200, 203, 214));
  for (i = 0; i < g_log_count; i++) {
    int failed = g_log[i][0] == 'F' && g_log[i][1] == 'A';
    fb_draw_text(cx + 18, cy + LOG_Y0 + i * 12, g_log[i],
                 failed ? FB_RGB(180, 30, 30) : FB_RGB(50, 55, 75), 1);
  }
}

// ---- input -----------------------------------------------------------------

static int in_box(int x, int y, int bx, int by, int bw, int bh) {
  return x >= bx && y >= by && x < bx + bw && y < by + bh;
}

static void setup_mouse(wm_window_t *win, int lx, int ly, int event) {
  int i;

  if (event != WM_MOUSE_DOWN || g_state == STATE_RUNNING) {
    return;
  }
  if (g_state == STATE_SELECT) {
    for (i = 0; i < disk_count(); i++) {
      if (in_box(lx, ly, 12, ROW_Y0 + i * ROW_H, 560, ROW_H - 2) && disk_selectable(i)) {
        g_selected = i;
        wm_invalidate_window(win);
        return;
      }
    }
    if (g_selected >= 0 && in_box(lx, ly, BTN_MAIN_X, BTN_Y, BTN_MAIN_W, BTN_H)) {
      g_state = STATE_CONFIRM;
      wm_invalidate_window(win);
    }
  } else if (g_state == STATE_CONFIRM) {
    if (in_box(lx, ly, BTN_MAIN_X, BTN_Y, BTN_MAIN_W, BTN_H)) {
      run_install(g_selected);
      wm_invalidate_window(win);
    } else if (in_box(lx, ly, BTN_CANCEL_X, BTN_Y, BTN_CANCEL_W, BTN_H)) {
      g_state = STATE_SELECT;
      wm_invalidate_window(win);
    }
  } else if (g_state == STATE_DONE) {
    if (in_box(lx, ly, BTN_MAIN_X, BTN_Y, BTN_MAIN_W, BTN_H)) {
      g_state = STATE_SELECT;
      g_selected = -1;
      g_log_count = 0;
      detect_boot_disk();
      wm_invalidate_window(win);
    }
  }
}

static void setup_closing(wm_window_t *win) {
  (void)win;
  g_win = 0;
}

void app_setup_open(void) {
  wm_window_t *win;

  if (g_win) {
    wm_restore(g_win);
    return;
  }
  win = wm_create("WinClone Setup", 100, 40, 580, 380);
  if (!win) {
    return;
  }
  g_win = win;
  g_state = STATE_SELECT;
  g_selected = -1;
  g_log_count = 0;
  detect_boot_disk();
  win->paint = setup_paint;
  win->mouse = setup_mouse;
  win->closing = setup_closing;
  win->min_w = 580;
  win->min_h = 380;
}
