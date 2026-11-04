#ifndef KERNEL_STRING_H
#define KERNEL_STRING_H

#include "console.h"

typedef unsigned long size_t;

// The compiler may emit calls to these even in freestanding code (struct
// copies, large initialisers), so they must exist.
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int value, size_t n);
int memcmp(const void *a, const void *b, size_t n);

size_t str_len(const char *s);
// Copy at most max-1 characters and always terminate. Returns the length.
size_t str_copy(char *dst, const char *src, size_t max);
// Case-insensitive (ASCII) comparison; returns 1 when equal.
int str_eq_nocase(const char *a, const char *b);
char char_upper(char c);
char char_lower(char c);
// Decimal text of v into out (needs 11 bytes). Returns the length.
int u32_to_str(uint32_t v, char *out);

#endif // KERNEL_STRING_H
