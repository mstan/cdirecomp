/*
 * cdi_nvram.c — DS1216 SmartWatch timekeeper + 32 KB NVRAM at $320000.
 *
 * Faithful C port of CeDImu's DS1216 core (external/CeDImu/src/CDI/cores/DS1216),
 * the behavioral oracle for the Mono-IV (Mono3) board cdi490a runs on. The chip
 * is a battery-backed 32 KB SRAM with an embedded RTC: reads/writes pass through
 * to SRAM until the host writes a fixed 64-bit magic pattern (to bit 0 of any
 * cell), which unlocks SERIAL access to 8 BCD clock registers (one bit per
 * access) for the next 64 accesses, then relocks.
 *
 * Bus wiring (Mono3, even bytes; see cdi_bus.c): the chip address is
 * (busaddr-$320000)>>1; byte access uses GetByte/SetByte directly, word access
 * puts the chip byte in the HIGH byte (GetByte<<8, SetByte(data>>8)).
 *
 * The clock is seeded from IRTC::defaultTime = 599616000 (1989-01-01 00:00:00),
 * the same deterministic time the oracle uses; SRAM is seeded to 0xFF (CeDImu's
 * empty-NVRAM default). The clock advances with EMULATED cycle time, exactly as
 * CeDImu's DS1216::IncrementClock does: nvram_increment_clock(ns) is driven by
 * mcd212_tick() with ns = cycles * (the SCC68070 cycle period), the same
 * per-instruction cycle-scaled quantity CeDImu's Interpreter feeds its
 * CDI::IncrementTime -> DS1216::IncrementClock chain (Interpreter.cpp:76-78,
 * CDI.cpp:108-112). This is deterministic (derived from the emulated cycle
 * count, never the host wall clock) so the co-sim stays reproducible; it is NOT
 * a frozen clock, so an RTC read's sub-second (and, on a long enough boot,
 * whole-second) fields depend on how many cycles have elapsed since reset —
 * matching the oracle bit-for-bit requires ticking on the same schedule.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "cdi_runtime.h"
#include <string.h>
#include <time.h>
#include <math.h>

/* DS1216 clock-register indices (CeDImu enum DS1216Clock). */
enum { Hundredths = 0, Seconds, Minutes, Hour, Day, Date, Month, Year };

/* The 64-bit unlock pattern ($C5,$3A,$A3,$5C twice), one bool per bit, exactly
 * as CeDImu's matchPattern (compared newest-bit-first against the rolling
 * history). */
static const uint8_t s_match[64] = {
    0,1,0,1,1,1,0,0,  1,0,1,0,0,0,1,1,
    0,0,1,1,1,0,1,0,  1,1,0,0,0,1,0,1,
    0,1,0,1,1,1,0,0,  1,0,1,0,0,0,1,1,
    0,0,1,1,1,0,1,0,  1,1,0,0,0,1,0,1,
};

static uint8_t  s_sram[0x8000];
static uint8_t  s_clock[8];
static uint64_t s_pattern;        /* rolling history: bit i = the bit pushed i steps ago */
static uint64_t s_magic;          /* s_match packed: bit i = s_match[i] */
static int      s_pattern_count;  /* <0 = SRAM mode; 0..63 = serial RTC access index */
static time_t   s_internal_time;
static double   s_nsec;           /* raw ns accumulator, always < 10'000'000 (CeDImu m_nsec) */
static uint32_t s_msec;           /* milliseconds within the current second, 0..999 —
                                    * the Hundredths register is byte_to_pbcd(s_msec/10), matching
                                    * CeDImu's hh_mm_ss(m_internalClock.time_since_epoch()).subseconds() */

static uint8_t byte_to_pbcd(uint8_t v) { v %= 100; return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static uint8_t pbcd_to_byte(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }

static time_t portable_timegm(struct tm *t) {
#ifdef _WIN32
    return _mkgmtime(t);
#else
    return timegm(t);
#endif
}

/* Internal clock -> SRAM clock registers (CeDImu DS1216::ClockToSRAM). */
static void clock_to_sram(void) {
    if (s_pattern_count >= 0) return;   /* never change the clock while it is being read */
    time_t t = s_internal_time;
    struct tm *g = gmtime(&t);
    if (!g) return;

    unsigned hour = (unsigned)(g->tm_hour % 24);
    uint8_t hreg;
    if (s_clock[Hour] & 0x80) {                 /* 12h format */
        int h = (int)hour;
        if (h >= 12) { hreg = 0xA0; if (h > 12) h -= 12; }  /* PM */
        else         { hreg = 0x80; if (h == 0) h = 12; }   /* AM */
        hreg |= byte_to_pbcd((uint8_t)h);
    } else {                                     /* 24h format */
        hreg = byte_to_pbcd((uint8_t)hour);
    }

    int iso = (g->tm_wday == 0) ? 7 : g->tm_wday;       /* weekday ISO: Mon=1..Sun=7 */
    s_clock[Hundredths] = byte_to_pbcd((uint8_t)(s_msec / 10));
    s_clock[Seconds]    = byte_to_pbcd((uint8_t)g->tm_sec);
    s_clock[Minutes]    = byte_to_pbcd((uint8_t)g->tm_min);
    s_clock[Hour]       = hreg;
    s_clock[Day]        = (uint8_t)(byte_to_pbcd((uint8_t)iso) | (s_clock[Day] & 0x30));
    s_clock[Date]       = byte_to_pbcd((uint8_t)g->tm_mday);
    s_clock[Month]      = byte_to_pbcd((uint8_t)(g->tm_mon + 1));
    s_clock[Year]       = byte_to_pbcd((uint8_t)((g->tm_year + 1900) % 100));
}

/* SRAM clock registers -> internal clock (CeDImu DS1216::SRAMToClock), used when
 * the host writes the RTC. */
static void sram_to_clock(void) {
    uint8_t hour;
    if (s_clock[Hour] & 0x80) {                  /* 12h */
        hour = pbcd_to_byte(s_clock[Hour] & 0x1F);
        if (hour == 12) hour = 0;
        if (s_clock[Hour] & 0x20) hour += 12;    /* PM */
    } else {
        hour = pbcd_to_byte(s_clock[Hour] & 0x3F);
    }
    struct tm tmv;
    memset(&tmv, 0, sizeof tmv);
    tmv.tm_sec  = pbcd_to_byte(s_clock[Seconds] & 0x7F);
    tmv.tm_min  = pbcd_to_byte(s_clock[Minutes] & 0x7F);
    tmv.tm_hour = hour;
    tmv.tm_mday = pbcd_to_byte(s_clock[Date] & 0x3F);
    tmv.tm_mon  = pbcd_to_byte(s_clock[Month] & 0x1F) - 1;
    {
        unsigned y = 1900 + pbcd_to_byte(s_clock[Year]);
        if (y < 1970) y += 100;                  /* two-digit year window 1970..2069 */
        tmv.tm_year = (int)y - 1900;
    }
    s_internal_time = portable_timegm(&tmv);
    /* CeDImu sets m_nsec (not the internal time_point's subsecond field) to the
     * written Hundredths value; the pending fractional second is drained back
     * out — 10ms at a time — by future IncrementClock calls, so the internal
     * clock itself starts this second at :00 (s_msec=0) and climbs back up. */
    s_nsec = pbcd_to_byte(s_clock[Hundredths]) * 10000000.0;
    s_msec = 0;
}

static void push_pattern(int bit) {
    s_pattern = (s_pattern << 1) | (uint64_t)(bit & 1);   /* bit 0 = newest */
    if (s_pattern == s_magic) {
        clock_to_sram();
        s_pattern_count = 0;
    }
}

static void increment_clock_access(void) {
    if (++s_pattern_count >= 64) s_pattern_count = -1;
}

/* Advance the internal clock by `ns` of EMULATED cycle time (CeDImu
 * DS1216::IncrementClock). Called once per instruction from mcd212_tick() with
 * ns = cycles * the SCC68070 cycle period — the same deterministic, cycle-
 * derived quantity CeDImu's Interpreter feeds through CDI::IncrementTime into
 * this same chain, never host wall-clock time. The OSC bit (Day reg bit 5)
 * stops the oscillator, exactly as on real hardware / CeDImu. */
void nvram_increment_clock(double ns) {
    if (s_clock[Day] & 0x20) return;       /* OSC bit set: clock halted */
    s_nsec += ns;
    while (s_nsec >= 10000000.0) {
        s_nsec -= 10000000.0;
        s_msec += 10;
        if (s_msec >= 1000) { s_msec -= 1000; s_internal_time += 1; }
    }
}

void nvram_reset(void) {
    memset(s_sram, 0xFF, sizeof s_sram);   /* CeDImu empty-NVRAM default */
    memset(s_clock, 0, sizeof s_clock);
    s_pattern = 0;
    s_pattern_count = -1;
    s_internal_time = 599616000;           /* IRTC::defaultTime: 1989-01-01 00:00:00 */
    s_nsec = 0.0;
    s_msec = 0;
    s_magic = 0;
    for (int i = 0; i < 64; i++) s_magic |= (uint64_t)(s_match[i] & 1) << i;
    clock_to_sram();
}

uint8_t nvram_get_byte(uint16_t dev) {
    if (s_pattern_count < 0)
        return s_sram[dev & 0x7FFF];
    {
        uint8_t reg   = (uint8_t)(s_pattern_count / 8);
        uint8_t shift = (uint8_t)(s_pattern_count % 8);
        uint8_t bit   = (uint8_t)((s_clock[reg] >> shift) & 1);
        increment_clock_access();
        return bit;
    }
}

void nvram_set_byte(uint16_t dev, uint8_t data) {
    int lsb = data & 1;
    if (s_pattern_count < 0) {
        s_sram[dev & 0x7FFF] = data;
        push_pattern(lsb);
    } else {
        uint8_t reg   = (uint8_t)(s_pattern_count / 8);
        uint8_t shift = (uint8_t)(s_pattern_count % 8);
        s_clock[reg] = (uint8_t)((s_clock[reg] & ~(1 << shift)) | (lsb << shift));
        increment_clock_access();
        if (s_pattern_count == -1) sram_to_clock();
    }
}
