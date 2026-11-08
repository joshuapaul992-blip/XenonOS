/* SPDX-License-Identifier: MIT */
/*
 * HAL - CMOS real-time clock, plus date conversion helpers.
 *
 * The RTC is read twice until two reads agree, so a read never mixes values
 * from before and after the clock ticks over. BCD and 12-hour formats are
 * handled, and the century comes from the CMOS register named in the FADT
 * when there is one. XenonOS keeps the RTC in UTC.
 */
#include "hal.h"
#include "ke.h"
#include "rtl.h"

#define CMOS_INDEX   0x70
#define CMOS_DATA    0x71
#define RTC_SECONDS  0x00
#define RTC_MINUTES  0x02
#define RTC_HOURS    0x04
#define RTC_WEEKDAY  0x06
#define RTC_DAY      0x07
#define RTC_MONTH    0x08
#define RTC_YEAR     0x09
#define RTC_STATUS_A 0x0A
#define RTC_STATUS_B 0x0B
#define STATUS_A_UIP 0x80
#define STATUS_B_SET 0x80
#define STATUS_B_24H 0x02
#define STATUS_B_BIN 0x04

static KSPIN_LOCK g_lock;

uint8_t HalCmosRead(uint8_t index)
{
    outb(CMOS_INDEX, index & 0x7F);     /* bit 7 clear: leave NMIs enabled */
    io_wait();
    return inb(CMOS_DATA);
}

static void cmos_write(uint8_t index, uint8_t v)
{
    outb(CMOS_INDEX, index & 0x7F);
    io_wait();
    outb(CMOS_DATA, v);
}

static uint8_t from_bcd(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t to_bcd(uint8_t v)   { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

typedef struct { uint8_t s, m, h, wd, d, mo, y, c; } RAW_TIME;

static void read_raw(RAW_TIME *r)
{
    for (int i = 0; i < 100000 && (HalCmosRead(RTC_STATUS_A) & STATUS_A_UIP); i++)
        cpu_pause();
    r->s = HalCmosRead(RTC_SECONDS);
    r->m = HalCmosRead(RTC_MINUTES);
    r->h = HalCmosRead(RTC_HOURS);
    r->wd = HalCmosRead(RTC_WEEKDAY);
    r->d = HalCmosRead(RTC_DAY);
    r->mo = HalCmosRead(RTC_MONTH);
    r->y = HalCmosRead(RTC_YEAR);
    r->c = HalAcpi.CenturyRegister ? HalCmosRead(HalAcpi.CenturyRegister) : 0;
}

bool HalQueryRealTimeClock(TIME_FIELDS *t)
{
    if (HalAcpi.BootArchFlags & ACPI_BOOT_ARCH_NO_CMOS_RTC)
        return false;
    RAW_TIME a, b;
    KIRQL_STATE s = KeAcquireSpinLock(&g_lock);
    read_raw(&a);
    for (int tries = 0; tries < 10; tries++) {
        read_raw(&b);
        if (!memcmp(&a, &b, sizeof(a)))
            break;
        a = b;
    }
    uint8_t status_b = HalCmosRead(RTC_STATUS_B);
    KeReleaseSpinLock(&g_lock, s);

    bool pm = a.h & 0x80;
    a.h &= 0x7F;
    if (!(status_b & STATUS_B_BIN)) {
        a.s = from_bcd(a.s); a.m = from_bcd(a.m); a.h = from_bcd(a.h);
        a.d = from_bcd(a.d); a.mo = from_bcd(a.mo); a.y = from_bcd(a.y);
        a.c = from_bcd(a.c);
    }
    if (!(status_b & STATUS_B_24H)) {
        if (a.h == 12) a.h = 0;
        if (pm) a.h += 12;
    }
    unsigned century = (a.c >= 19 && a.c <= 30) ? a.c : (a.y < 80 ? 20 : 19);
    t->Year = (uint16_t)(century * 100 + a.y);
    t->Month = a.mo;
    t->Day = a.d;
    t->Hour = a.h;
    t->Minute = a.m;
    t->Second = a.s;
    if (t->Month < 1 || t->Month > 12 || t->Day < 1 || t->Day > 31 || t->Hour > 23 ||
        t->Minute > 59 || t->Second > 60)
        return false;
    /* Compute the weekday instead of trusting the CMOS (many BIOSes leave it wrong). */
    TIME_FIELDS tmp;
    RtlUnixToTimeFields(RtlTimeFieldsToUnix(t), &tmp);
    t->Weekday = tmp.Weekday;
    return true;
}

bool HalSetRealTimeClock(const TIME_FIELDS *t)
{
    if (t->Year < 1980 || t->Year > 2199 || t->Month < 1 || t->Month > 12 || t->Day < 1 ||
        t->Day > 31 || t->Hour > 23 || t->Minute > 59 || t->Second > 59)
        return false;
    KIRQL_STATE s = KeAcquireSpinLock(&g_lock);
    uint8_t status_b = HalCmosRead(RTC_STATUS_B);
    bool bin = status_b & STATUS_B_BIN;
    bool h24 = status_b & STATUS_B_24H;
    cmos_write(RTC_STATUS_B, status_b | STATUS_B_SET);      /* stop updates while writing */

    uint8_t hour = t->Hour;
    bool pm = false;
    if (!h24) {
        pm = hour >= 12;
        hour = hour % 12;
        if (hour == 0) hour = 12;
    }
    uint8_t year = (uint8_t)(t->Year % 100), century = (uint8_t)(t->Year / 100);
    TIME_FIELDS tmp;
    RtlUnixToTimeFields(RtlTimeFieldsToUnix(t), &tmp);
#define ENC(v) (bin ? (uint8_t)(v) : to_bcd((uint8_t)(v)))
    cmos_write(RTC_SECONDS, ENC(t->Second));
    cmos_write(RTC_MINUTES, ENC(t->Minute));
    cmos_write(RTC_HOURS, (uint8_t)(ENC(hour) | (pm ? 0x80 : 0)));
    cmos_write(RTC_WEEKDAY, ENC(tmp.Weekday + 1));
    cmos_write(RTC_DAY, ENC(t->Day));
    cmos_write(RTC_MONTH, ENC(t->Month));
    cmos_write(RTC_YEAR, ENC(year));
    if (HalAcpi.CenturyRegister)
        cmos_write(HalAcpi.CenturyRegister, ENC(century));
#undef ENC
    cmos_write(RTC_STATUS_B, status_b & ~STATUS_B_SET);
    KeReleaseSpinLock(&g_lock, s);
    return true;
}

/* ---- Date arithmetic (proleptic Gregorian calendar) ----------------------- */

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

int64_t RtlTimeFieldsToUnix(const TIME_FIELDS *t)
{
    return days_from_civil(t->Year, t->Month, t->Day) * 86400 +
           (int64_t)t->Hour * 3600 + t->Minute * 60 + t->Second;
}

void RtlUnixToTimeFields(int64_t secs, TIME_FIELDS *t)
{
    int64_t days = secs >= 0 ? secs / 86400 : -((-secs + 86399) / 86400);
    int64_t rem = secs - days * 86400;
    t->Hour = (uint8_t)(rem / 3600);
    t->Minute = (uint8_t)((rem % 3600) / 60);
    t->Second = (uint8_t)(rem % 60);
    t->Weekday = (uint8_t)((days % 7 + 11) % 7);         /* 1970-01-01 was a Thursday */

    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    t->Day = (uint8_t)(doy - (153 * mp + 2) / 5 + 1);
    t->Month = (uint8_t)(mp < 10 ? mp + 3 : mp - 9);
    t->Year = (uint16_t)(y + (t->Month <= 2));
}
