#include "rtc.h"

#define CMOS_INDEX  0x70
#define CMOS_DATA   0x71
#define RTC_SECONDS 0x00
#define RTC_MINUTES 0x02
#define RTC_HOURS   0x04
#define RTC_STATUS_A 0x0A
#define RTC_STATUS_B 0x0B

static inline void outb(uint16_t port, uint8_t value) {
  __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
  uint8_t value;
  __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
  return value;
}

static uint8_t cmos_read(uint8_t reg) {
  outb(CMOS_INDEX, reg);     // bit 7 clear keeps NMI enabled
  return inb(CMOS_DATA);
}

static int update_in_progress(void) {
  return (cmos_read(RTC_STATUS_A) & 0x80) != 0;
}

static int from_bcd(int v) {
  return (v & 0x0F) + ((v >> 4) * 10);
}

int rtc_read_time(int *hour, int *minute, int *second) {
  int h, m, s, h2, m2, s2, spins = 0;
  uint8_t status_b;

  // Read twice until two reads agree, avoiding a mid-update value.
  do {
    while (update_in_progress() && spins++ < 100000) {
    }
    s = cmos_read(RTC_SECONDS);
    m = cmos_read(RTC_MINUTES);
    h = cmos_read(RTC_HOURS);
    while (update_in_progress() && spins++ < 100000) {
    }
    s2 = cmos_read(RTC_SECONDS);
    m2 = cmos_read(RTC_MINUTES);
    h2 = cmos_read(RTC_HOURS);
  } while ((s != s2 || m != m2 || h != h2) && spins++ < 200000);

  status_b = cmos_read(RTC_STATUS_B);
  if (!(status_b & 0x04)) {          // BCD mode
    int pm = h & 0x80;
    s = from_bcd(s);
    m = from_bcd(m);
    h = from_bcd(h & 0x7F);
    if (!(status_b & 0x02) && pm) {  // 12-hour clock with PM bit
      h = (h % 12) + 12;
    }
  } else if (!(status_b & 0x02) && (h & 0x80)) {
    h = ((h & 0x7F) % 12) + 12;
  }
  if (h > 23 || m > 59 || s > 59) {
    return 0;
  }
  *hour = h;
  *minute = m;
  *second = s;
  return 1;
}

int rtc_read_date(int *year, int *month, int *day) {
  int y, m, d, spins = 0;
  uint8_t status_b;

  while (update_in_progress() && spins++ < 100000) {
  }
  d = cmos_read(0x07);
  m = cmos_read(0x08);
  y = cmos_read(0x09);
  status_b = cmos_read(RTC_STATUS_B);
  if (!(status_b & 0x04)) {
    d = from_bcd(d);
    m = from_bcd(m);
    y = from_bcd(y);
  }
  if (m < 1 || m > 12 || d < 1 || d > 31) {
    return 0;
  }
  *year = 2000 + y;
  *month = m;
  *day = d;
  return 1;
}

static int days_in_month(int year, int month) {
  static const char dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  return (month == 2 && leap) ? 29 : dim[month - 1];
}

int rtc_read_local(int offset_min, int *year, int *month, int *day,
                   int *hour, int *minute) {
  int h, m, s, y, mo, d, total;

  // Read the date on both sides of the time so a midnight rollover between
  // the two reads cannot pair a new time with the old date.
  if (!rtc_read_date(&y, &mo, &d) || !rtc_read_time(&h, &m, &s)) {
    return 0;
  }
  {
    int y2, mo2, d2;
    if (rtc_read_date(&y2, &mo2, &d2) && (d2 != d || mo2 != mo || y2 != y)) {
      y = y2; mo = mo2; d = d2;
      if (!rtc_read_time(&h, &m, &s)) {
        return 0;
      }
    }
  }
  total = h * 60 + m + offset_min;
  while (total < 0) {
    total += 24 * 60;
    if (--d < 1) {
      if (--mo < 1) { mo = 12; y--; }
      d = days_in_month(y, mo);
    }
  }
  while (total >= 24 * 60) {
    total -= 24 * 60;
    if (++d > days_in_month(y, mo)) {
      d = 1;
      if (++mo > 12) { mo = 1; y++; }
    }
  }
  *year = y; *month = mo; *day = d;
  *hour = total / 60; *minute = total % 60;
  return 1;
}
