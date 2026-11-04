#ifndef KERNEL_CONSOLE_H
#define KERNEL_CONSOLE_H

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef unsigned long uintptr_t;

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

#endif // KERNEL_CONSOLE_H
