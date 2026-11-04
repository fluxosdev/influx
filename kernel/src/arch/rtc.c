#include "arch.h"
#include "io.h"
#include "spinlock.h"

/* The index/data pairs are shared by the RTC and AML CMOS regions, which may
 * run on different CPUs: one lock keeps an index and its data access together.
 * Port 0x70 is write-only (reads give 0xFF on QEMU and most chipsets), so its
 * NMI-disable bit 7 cannot be preserved; it is always written clear. Bank 1
 * takes a 7-bit index too: 0x72 aliases 0x70 when the upper bank is disabled. */
static spinlock_t cmos_lock = SPINLOCK_INIT;

static uint16_t cmos_select(uint8_t reg) {
    outb(reg < 128 ? 0x70 : 0x72, reg & 0x7F);
    return reg < 128 ? 0x71 : 0x73;
}
uint8_t cmos_read(uint8_t reg) {
    spin_lock(&cmos_lock);
    uint8_t v = inb(cmos_select(reg));
    spin_unlock(&cmos_lock);
    return v;
}
void cmos_write(uint8_t reg, uint8_t v) {
    spin_lock(&cmos_lock);
    outb(cmos_select(reg), v);
    spin_unlock(&cmos_lock);
}
static int bcd(uint8_t v) { return (v & 0x0F) + ((v >> 4) * 10); }

void rtc_read(struct rtc_time *t) {
    /* update in progress lasts about 2 ms; an absent RTC reads 0xFF forever, so give up after
     * about a second (each port read is ~1 us) rather than hang timer_init silently */
    for (int i = 0; i < 1000000 && (cmos_read(0x0A) & 0x80); i++) pause();
    uint8_t s = cmos_read(0x00), m = cmos_read(0x02), h = cmos_read(0x04);
    uint8_t d = cmos_read(0x07), mo = cmos_read(0x08), y = cmos_read(0x09);
    uint8_t b = cmos_read(0x0B);
    bool is_bcd = !(b & 4), is_12h = !(b & 2);
    bool pm = h & 0x80; h &= 0x7F;
    if (is_bcd) { s = bcd(s); m = bcd(m); h = bcd(h); d = bcd(d); mo = bcd(mo); y = bcd(y); }
    if (is_12h) { if (h == 12) h = 0; if (pm) h += 12; }
    t->second = s; t->minute = m; t->hour = h; t->day = d; t->month = mo; t->year = 2000 + y;
}

static uint8_t to_bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

void rtc_write(const struct rtc_time *t) {
    if (t->year < 2000 || t->year > 2099) return;
    uint8_t b = cmos_read(0x0B);
    if (b == 0xFF) return;   /* no RTC */
    bool is_bcd = !(b & 4), is_12h = !(b & 2);
    int h = t->hour;
    uint8_t hv;
    if (is_12h) {
        bool pm = h >= 12; int h12 = h % 12 ? h % 12 : 12;
        hv = (uint8_t)((is_bcd ? to_bcd(h12) : h12) | (pm ? 0x80 : 0));
    } else hv = (uint8_t)(is_bcd ? to_bcd(h) : h);
#define V(x) ((uint8_t)(is_bcd ? to_bcd(x) : (x)))
    cmos_write(0x0B, b | 0x80);   /* SET: halt updates while the fields change */
    cmos_write(0x00, V(t->second)); cmos_write(0x02, V(t->minute)); cmos_write(0x04, hv);
    cmos_write(0x07, V(t->day)); cmos_write(0x08, V(t->month)); cmos_write(0x09, V(t->year - 2000));
    if (t->day >= 1 && t->month >= 1) cmos_write(0x06, (uint8_t)(day_of_week(t->year, t->month, t->day) + 1));
#undef V
    cmos_write(0x0B, b & ~0x80);
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
