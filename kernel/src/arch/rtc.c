#include "arch.h"
#include "io.h"

static uint8_t cmos_read(uint8_t reg) { outb(0x70, reg); return inb(0x71); }
static int bcd(uint8_t v) { return (v & 0x0F) + ((v >> 4) * 10); }

void rtc_read(struct rtc_time *t) {
    while (cmos_read(0x0A) & 0x80) pause();
    uint8_t s = cmos_read(0x00), m = cmos_read(0x02), h = cmos_read(0x04);
    uint8_t d = cmos_read(0x07), mo = cmos_read(0x08), y = cmos_read(0x09);
    uint8_t b = cmos_read(0x0B);
    bool is_bcd = !(b & 4), is_12h = !(b & 2);
    bool pm = h & 0x80; h &= 0x7F;
    if (is_bcd) { s = bcd(s); m = bcd(m); h = bcd(h); d = bcd(d); mo = bcd(mo); y = bcd(y); }
    if (is_12h) { if (h == 12) h = 0; if (pm) h += 12; }
    t->second = s; t->minute = m; t->hour = h; t->day = d; t->month = mo; t->year = 2000 + y;
}

static int days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

uint64_t rtc_to_unix(const struct rtc_time *t) {
    return (uint64_t)days_from_civil(t->year, t->month, t->day) * 86400 + t->hour * 3600 + t->minute * 60 + t->second;
}

void unix_to_datetime(uint64_t ts, struct rtc_time *out) {
    int64_t z = (int64_t)(ts / 86400) + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp + (mp < 10 ? 3 : -9);
    out->year = (int)(y + (m <= 2)); out->month = (int)m; out->day = (int)d;
    uint64_t rem = ts % 86400;
    out->hour = rem / 3600; out->minute = (rem % 3600) / 60; out->second = rem % 60;
}

int day_of_week(int y, int m, int d) { /* 0 = Sunday */
    int days = days_from_civil(y, m, d);
    return ((days % 7) + 11) % 7;   /* 1970-01-01 was a Thursday (4) */
}
