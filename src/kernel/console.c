#include "console.h"

// UEFI types and structures
typedef unsigned char UINT8;
typedef unsigned short UINT16;
typedef unsigned int UINT32;
typedef unsigned long long UINT64;
typedef unsigned long long UINTN;
typedef void *EFI_HANDLE;
#define EFIAPI __attribute__((ms_abi))

typedef struct {
  UINT64 Signature;
  UINT32 Revision;
  UINT32 HeaderSize;
  UINT32 CRC32;
  UINT32 Reserved;
} EFI_TABLE_HEADER;

typedef struct SIMPLE_TEXT_OUTPUT_PROTOCOL SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef UINT64(EFIAPI *EFI_TEXT_STRING)(SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                        UINT16 *String);

struct SIMPLE_TEXT_OUTPUT_PROTOCOL {
  void *Reset;
  EFI_TEXT_STRING OutputString;
};

struct EFI_SYSTEM_TABLE {
  EFI_TABLE_HEADER Hdr;
  UINT16 *FirmwareVendor;
  UINT32 FirmwareRevision;
  UINT32 ReservedPad;
  EFI_HANDLE ConsoleInHandle;
  void *ConIn;
  EFI_HANDLE ConsoleOutHandle;
  SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
  EFI_HANDLE StandardErrorHandle;
  SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
  void *RuntimeServices;
  void *BootServices;
  UINTN NumberOfTableEntries;
  void *ConfigurationTable;
};

static EFI_SYSTEM_TABLE *g_system_table = 0;
static int g_serial_ready = 0;

#define COM1_PORT 0x3F8U

static unsigned char inb(unsigned short port) {
  unsigned char value;
  __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
  return value;
}

static void outb(unsigned short port, unsigned char value) {
  __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static void serial_init(void) {
  outb(COM1_PORT + 1U, 0x00U);
  outb(COM1_PORT + 3U, 0x80U);
  outb(COM1_PORT + 0U, 0x03U);
  outb(COM1_PORT + 1U, 0x00U);
  outb(COM1_PORT + 3U, 0x03U);
  outb(COM1_PORT + 2U, 0xC7U);
  outb(COM1_PORT + 4U, 0x0BU);
  g_serial_ready = 1;
}

static int serial_is_transmit_empty(void) {
  return (inb(COM1_PORT + 5U) & 0x20U) != 0;
}

static void serial_write_char(char c) {
  if (!g_serial_ready) {
    return;
  }

  while (!serial_is_transmit_empty()) {
  }
  outb(COM1_PORT, (unsigned char)c);
}

void console_init(EFI_SYSTEM_TABLE *system_table) {
  g_system_table = system_table;
  serial_init();
}

void console_write_wide_string(const uint16_t *str) {
  if (g_system_table == 0 || g_system_table->ConOut == 0) {
    return;
  }
  
  if (str == 0) {
    return;
  }
  
  // Call UEFI OutputString
  g_system_table->ConOut->OutputString(g_system_table->ConOut, (UINT16 *)str);
}

// Convert ASCII string to UTF-16
static uint16_t *ascii_to_wide(const char *ascii) {
  static uint16_t buffer[512];
  int i = 0;
  
  if (ascii == 0) {
    return 0;
  }
  
  while (ascii[i] && i < 510) {
    buffer[i] = (uint16_t)ascii[i];
    i++;
  }
  buffer[i] = 0;
  
  return buffer;
}

void console_write_string(const char *str) {
  const char *original;
  uint16_t *wide;

  if (str == 0) {
    return;
  }

  original = str;
  while (*str) {
    serial_write_char(*str++);
  }

  if (!g_serial_ready) {
    wide = ascii_to_wide(original);
    console_write_wide_string(wide);
  }
}

void console_write_char(char c) {
  char str[2] = {c, 0};
  console_write_string(str);
}

void console_newline(void) {
  console_write_string("\r\n");
}

// Simple printf implementation
void console_printf(const char *fmt, ...) {
  if (fmt == 0) return;

  __builtin_va_list args;
  __builtin_va_start(args, fmt);

  const char *p = fmt;
  while (*p) {
    if (*p != '%') {
      console_write_char(*p++);
      continue;
    }
    ++p;  // skip '%'

    // Handle %ll prefix
    int is_long_long = 0;
    if (p[0] == 'l' && p[1] == 'l') {
      is_long_long = 1;
      p += 2;
    }

    switch (*p) {
      case 's': {
        const char *s = __builtin_va_arg(args, const char *);
        console_write_string(s ? s : "(null)");
        break;
      }
      case 'd':
      case 'i': {
        long long v = is_long_long ? __builtin_va_arg(args, long long)
                                   : (long long)__builtin_va_arg(args, int);
        if (v < 0) { console_write_char('-'); v = -v; }
        char buf[21]; int idx = 0;
        unsigned long long u = (unsigned long long)v;
        if (u == 0) { console_write_char('0'); break; }
        while (u) { buf[idx++] = '0' + (int)(u % 10); u /= 10; }
        while (idx--) console_write_char(buf[idx]);
        break;
      }
      case 'u': {
        unsigned long long v = is_long_long
          ? __builtin_va_arg(args, unsigned long long)
          : (unsigned long long)__builtin_va_arg(args, unsigned int);
        char buf[21]; int idx = 0;
        if (v == 0) { console_write_char('0'); break; }
        while (v) { buf[idx++] = '0' + (int)(v % 10); v /= 10; }
        while (idx--) console_write_char(buf[idx]);
        break;
      }
      case 'x':
      case 'X':
      case 'p': {
        unsigned long long v = is_long_long || *p == 'p'
          ? __builtin_va_arg(args, unsigned long long)
          : (unsigned long long)__builtin_va_arg(args, unsigned int);
        const char *hex = (*p == 'X') ? "0123456789ABCDEF"
                                      : "0123456789abcdef";
        if (*p == 'p') console_write_string("0x");
        char buf[17]; int idx = 0;
        if (v == 0) { console_write_char('0'); break; }
        while (v) { buf[idx++] = hex[v & 0xF]; v >>= 4; }
        while (idx--) console_write_char(buf[idx]);
        break;
      }
      case '%':
        console_write_char('%');
        break;
      default:
        console_write_char('%');
        console_write_char(*p);
        break;
    }
    ++p;
  }

  __builtin_va_end(args);
}
