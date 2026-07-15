#include "cdi_host_time.h"

#ifdef _WIN32
#include <windows.h>

int cdi_host_local_time(CdiRtcTime *time_value) {
    SYSTEMTIME local;
    GetLocalTime(&local);
    time_value->year = local.wYear;
    time_value->month = (uint8_t)local.wMonth;
    time_value->date = (uint8_t)local.wDay;
    time_value->weekday = (uint8_t)(local.wDayOfWeek == 0 ? 7 : local.wDayOfWeek);
    time_value->hour = (uint8_t)local.wHour;
    time_value->minute = (uint8_t)local.wMinute;
    time_value->second = (uint8_t)local.wSecond;
    time_value->hundredth = (uint8_t)(local.wMilliseconds / 10);
    return 1;
}
#else
#include <time.h>

int cdi_host_local_time(CdiRtcTime *time_value) {
    struct timespec now;
    struct tm local;
    if (timespec_get(&now, TIME_UTC) != TIME_UTC) return 0;
    if (!localtime_r(&now.tv_sec, &local)) return 0;
    time_value->year = local.tm_year + 1900;
    time_value->month = (uint8_t)(local.tm_mon + 1);
    time_value->date = (uint8_t)local.tm_mday;
    time_value->weekday = (uint8_t)(local.tm_wday == 0 ? 7 : local.tm_wday);
    time_value->hour = (uint8_t)local.tm_hour;
    time_value->minute = (uint8_t)local.tm_min;
    time_value->second = (uint8_t)local.tm_sec;
    time_value->hundredth = (uint8_t)(now.tv_nsec / 10000000L);
    return 1;
}
#endif
