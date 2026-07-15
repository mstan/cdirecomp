/*
 * DS1216-compatible phantom clock and 32 KiB battery-backed SRAM.
 *
 * Source basis: Dallas/Maxim DS1216 data sheet, revision 2.  The device acts
 * like ordinary SRAM until the documented 64-bit comparison sequence is
 * written through D0.  The next 64 accesses shift the eight clock registers
 * least-significant bit first, after which normal SRAM access resumes.
 *
 * The implementation keeps civil time directly instead of translating
 * through a host time library.  That makes reset and cycle-derived advancement
 * deterministic on every host and keeps the RTC independent of wall time.
 */
#include "cdi_runtime.h"

#include <stdint.h>
#include <string.h>

enum {
    RTC_HUNDREDTHS,
    RTC_SECONDS,
    RTC_MINUTES,
    RTC_HOURS,
    RTC_WEEKDAY,
    RTC_DATE,
    RTC_MONTH,
    RTC_YEAR,
    RTC_REGISTER_COUNT
};

enum { NVRAM_BYTES = 0x8000 };

/* C5 3A A3 5C C5 3A A3 5C, with every byte presented D0 first. */
static const uint8_t unlock_bytes[8] = {
    0xC5, 0x3A, 0xA3, 0x5C, 0xC5, 0x3A, 0xA3, 0x5C
};

typedef struct {
    int year;              /* full Gregorian year */
    uint8_t month;         /* 1..12 */
    uint8_t date;          /* 1..31 */
    uint8_t weekday;       /* 1=Monday .. 7=Sunday */
    uint8_t hour;          /* 0..23 */
    uint8_t minute;
    uint8_t second;
    uint8_t hundredth;
} CivilClock;

typedef struct {
    uint8_t ram[NVRAM_BYTES];
    uint8_t transfer[RTC_REGISTER_COUNT];
    CivilClock now;
    uint64_t key_window;
    uint64_t key_value;
    double sub_hundredth_ns;
    uint8_t transfer_bit;
    uint8_t transferring;
} PhantomClock;

static PhantomClock rtc;

static uint8_t bcd_encode(unsigned value) {
    return (uint8_t)(((value / 10u) << 4) | (value % 10u));
}

static unsigned bcd_decode(uint8_t value) {
    return (unsigned)(value >> 4) * 10u + (unsigned)(value & 0x0Fu);
}

static int leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static unsigned days_in_month(int year, unsigned month) {
    static const uint8_t lengths[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    if (month == 2) return lengths[1] + (unsigned)leap_year(year);
    if (month >= 1 && month <= 12) return lengths[month - 1];
    return 31;
}

static unsigned bounded(unsigned value, unsigned low, unsigned high,
                        unsigned fallback) {
    return value >= low && value <= high ? value : fallback;
}

static void advance_one_second(CivilClock *clock) {
    if (++clock->second < 60) return;
    clock->second = 0;
    if (++clock->minute < 60) return;
    clock->minute = 0;
    if (++clock->hour < 24) return;
    clock->hour = 0;
    clock->weekday = (uint8_t)(clock->weekday == 7 ? 1 : clock->weekday + 1);
    if (++clock->date <= days_in_month(clock->year, clock->month)) return;
    clock->date = 1;
    if (++clock->month <= 12) return;
    clock->month = 1;
    clock->year++;
}

static void latch_clock_registers(void) {
    uint8_t hour_mode = rtc.transfer[RTC_HOURS] & 0x80u;
    uint8_t day_control = rtc.transfer[RTC_WEEKDAY] & 0x30u;
    unsigned display_hour = rtc.now.hour;
    uint8_t encoded_hour;

    if (hour_mode) {
        int pm = display_hour >= 12;
        display_hour %= 12;
        if (display_hour == 0) display_hour = 12;
        encoded_hour = (uint8_t)(0x80u | (pm ? 0x20u : 0u) |
                                 bcd_encode(display_hour));
    } else {
        encoded_hour = bcd_encode(display_hour);
    }

    rtc.transfer[RTC_HUNDREDTHS] = bcd_encode(rtc.now.hundredth);
    rtc.transfer[RTC_SECONDS] = bcd_encode(rtc.now.second);
    rtc.transfer[RTC_MINUTES] = bcd_encode(rtc.now.minute);
    rtc.transfer[RTC_HOURS] = encoded_hour;
    rtc.transfer[RTC_WEEKDAY] = (uint8_t)(day_control | rtc.now.weekday);
    rtc.transfer[RTC_DATE] = bcd_encode(rtc.now.date);
    rtc.transfer[RTC_MONTH] = bcd_encode(rtc.now.month);
    rtc.transfer[RTC_YEAR] = bcd_encode((unsigned)(rtc.now.year % 100));
}

static void commit_clock_registers(void) {
    unsigned hour;
    unsigned year = bcd_decode(rtc.transfer[RTC_YEAR]);

    if (rtc.transfer[RTC_HOURS] & 0x80u) {
        hour = bounded(bcd_decode(rtc.transfer[RTC_HOURS] & 0x1Fu), 1, 12, 12);
        if (hour == 12) hour = 0;
        if (rtc.transfer[RTC_HOURS] & 0x20u) hour += 12;
    } else {
        hour = bounded(bcd_decode(rtc.transfer[RTC_HOURS] & 0x3Fu), 0, 23, 0);
    }

    rtc.now.year = (int)(year < 70 ? 2000u + year : 1900u + year);
    rtc.now.month = (uint8_t)bounded(bcd_decode(rtc.transfer[RTC_MONTH] & 0x1Fu),
                                     1, 12, 1);
    rtc.now.date = (uint8_t)bounded(bcd_decode(rtc.transfer[RTC_DATE] & 0x3Fu),
                                    1, days_in_month(rtc.now.year, rtc.now.month), 1);
    rtc.now.weekday = (uint8_t)bounded(rtc.transfer[RTC_WEEKDAY] & 7u, 1, 7, 1);
    rtc.now.hour = (uint8_t)hour;
    rtc.now.minute = (uint8_t)bounded(bcd_decode(rtc.transfer[RTC_MINUTES] & 0x7Fu),
                                      0, 59, 0);
    rtc.now.second = (uint8_t)bounded(bcd_decode(rtc.transfer[RTC_SECONDS] & 0x7Fu),
                                      0, 59, 0);
    rtc.now.hundredth = (uint8_t)bounded(bcd_decode(rtc.transfer[RTC_HUNDREDTHS]),
                                         0, 99, 0);
    rtc.sub_hundredth_ns = 0.0;
}

static uint64_t make_unlock_value(void) {
    uint64_t value = 0;
    unsigned byte_index;
    for (byte_index = 0; byte_index < 8; byte_index++) {
        unsigned bit;
        for (bit = 0; bit < 8; bit++)
            value = (value << 1) | ((unlock_bytes[byte_index] >> bit) & 1u);
    }
    return value;
}

static void observe_key_bit(unsigned bit) {
    rtc.key_window = (rtc.key_window << 1) | (uint64_t)(bit & 1u);
    if (rtc.key_window == rtc.key_value) {
        latch_clock_registers();
        rtc.key_window = 0;
        rtc.transfer_bit = 0;
        rtc.transferring = 1;
    }
}

static void finish_transfer_bit(void) {
    rtc.transfer_bit++;
    if (rtc.transfer_bit == 64) {
        rtc.transferring = 0;
        rtc.transfer_bit = 0;
        rtc.key_window = 0;
        commit_clock_registers();
    }
}

void nvram_reset(void) {
    memset(&rtc, 0, sizeof rtc);
    memset(rtc.ram, 0xFF, sizeof rtc.ram);
    rtc.key_value = make_unlock_value();
    rtc.now.year = 1989;
    rtc.now.month = 1;
    rtc.now.date = 1;
    rtc.now.weekday = 7; /* 1989-01-01 was Sunday. */
    latch_clock_registers();
}

void nvram_increment_clock(double nanoseconds) {
    if (rtc.transfer[RTC_WEEKDAY] & 0x20u) return; /* oscillator disabled */
    rtc.sub_hundredth_ns += nanoseconds;
    while (rtc.sub_hundredth_ns >= 10000000.0) {
        rtc.sub_hundredth_ns -= 10000000.0;
        if (++rtc.now.hundredth == 100) {
            rtc.now.hundredth = 0;
            advance_one_second(&rtc.now);
        }
    }
}

uint8_t nvram_get_byte(uint16_t device_address) {
    if (!rtc.transferring)
        return rtc.ram[device_address & (NVRAM_BYTES - 1)];

    {
        unsigned reg = rtc.transfer_bit >> 3;
        unsigned shift = rtc.transfer_bit & 7u;
        uint8_t result = (uint8_t)((rtc.transfer[reg] >> shift) & 1u);
        finish_transfer_bit();
        return result;
    }
}

void nvram_set_byte(uint16_t device_address, uint8_t data) {
    if (!rtc.transferring) {
        rtc.ram[device_address & (NVRAM_BYTES - 1)] = data;
        observe_key_bit(data);
        return;
    }

    {
        unsigned reg = rtc.transfer_bit >> 3;
        unsigned shift = rtc.transfer_bit & 7u;
        uint8_t mask = (uint8_t)(1u << shift);
        rtc.transfer[reg] = (uint8_t)((rtc.transfer[reg] & ~mask) |
                                      ((data & 1u) ? mask : 0u));
        finish_transfer_bit();
    }
}
