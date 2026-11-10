#ifndef KERNEL_CONSOLE_H
#define KERNEL_CONSOLE_H

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef unsigned long uintptr_t;
typedef long long int64_t;

// Forward declare UEFI types
typedef struct EFI_SYSTEM_TABLE EFI_SYSTEM_TABLE;

// Initialize console with UEFI system table
void console_init(EFI_SYSTEM_TABLE *system_table);

// Output a null-terminated string
void console_write_string(const char *str);

// Output a null-terminated UTF-16 wide string
void console_write_wide_string(const uint16_t *str);

// Output a single character
void console_write_char(char c);

// Formatted printing (like printf)
void console_printf(const char *fmt, ...);

// Output a newline
void console_newline(void);

// Everything written through console_write_string / console_printf is also kept in a
// ring buffer (the last CONSOLE_LOG_SIZE bytes), which the on-screen Console window
// shows. Positions are running byte counts since boot.
#define CONSOLE_LOG_SIZE 32768U
// Total bytes logged so far; the oldest byte still held is at total - CONSOLE_LOG_SIZE
// (or 0 while less has been logged).
uint64_t console_log_total(void);
// Copy up to max bytes starting at position `from` (clamped to what is still held).
// Returns the number copied and sets *actual_from to where the copy really began.
uint64_t console_log_read(uint64_t from, char *buf, uint64_t max, uint64_t *actual_from);
// Forget everything logged so far (the total keeps counting).
void console_log_clear(void);

#endif // KERNEL_CONSOLE_H
