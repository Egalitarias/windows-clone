#ifndef KERNEL_RTC_H
#define KERNEL_RTC_H

#include "console.h"

// CMOS real-time clock (wall-clock time as set by firmware / the hypervisor;
// usually UTC). Returns 1 on success.
int rtc_read_time(int *hour, int *minute, int *second);

// Calendar date from the same clock. Returns 1 on success.
int rtc_read_date(int *year, int *month, int *day);

// Date and time shifted by offset_min minutes (the local time zone), with the
// day, month and year rolled over as needed. Returns 1 on success.
int rtc_read_local(int offset_min, int *year, int *month, int *day,
                   int *hour, int *minute);

#endif // KERNEL_RTC_H
