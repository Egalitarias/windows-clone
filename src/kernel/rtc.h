#ifndef KERNEL_RTC_H
#define KERNEL_RTC_H

#include "console.h"

// CMOS real-time clock (wall-clock time as set by firmware / the hypervisor;
// usually UTC). Returns 1 on success.
int rtc_read_time(int *hour, int *minute, int *second);

// Calendar date from the same clock. Returns 1 on success.
int rtc_read_date(int *year, int *month, int *day);

#endif // KERNEL_RTC_H
