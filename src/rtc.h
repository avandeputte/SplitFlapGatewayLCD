// rtc.h -- wall-clock time: the ESP32-P4's system clock + NTP.
//
// This board has no battery-backed RTC chip, so the clock is invalid until the
// first NTP sync -- every caller already handles rtcNow.valid == false and
// rtcEpochNow() == 0. Local time comes from the configured POSIX TZ at format time.

#ifndef MPGW_RTC_H
#define MPGW_RTC_H

#include "common.h"
#include <time.h>

struct RtcTime {
  uint16_t year;
  uint8_t  month, day, hour, minute, second;
  bool     valid;
};

// ---- owned globals (defined in globals.cpp) ----
extern volatile RtcTime rtcNow;
extern char gPosixTZ[64];

void rtcHwInit();
void rtcRead();
bool rtcNTPSync();
void rtcFormatTime(char* out, size_t outLen);
unsigned long rtcEpochNow();
bool rtcLocalNow(struct tm* out);   // broken-down local time; false if clock unset or TZ lock busy
void cfgApplyTZ();                   // (re)apply gPosixTZ to the process env, serialised on timeMutex

#endif // MPGW_RTC_H
