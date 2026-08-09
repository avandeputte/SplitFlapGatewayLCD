#include "common.h"
#include "battery.h"

#if BOARD_HAS_BATTERY

static uint16_t gMv     = 0;      // smoothed pack voltage (mV)
static uint8_t  gPct    = 0;
static uint32_t gAt     = 0;      // millis() of last sample
static bool     gPrimed = false;

// Resting single-cell Li-ion voltage -> % (piecewise-linear, approximate). Under charge or load
// the curve shifts, so this is a rough fuel gauge, not a coulomb count.
static uint8_t pctFromMv(uint16_t mv) {
  static const uint16_t v[] = {3000, 3300, 3500, 3700, 3800, 3900, 4000, 4100, 4200};
  static const uint8_t  p[] = {   0,    5,   10,   35,   50,   65,   80,   90,  100};
  if (mv <= v[0]) return 0;
  if (mv >= v[8]) return 100;
  for (int i = 0; i < 8; i++)
    if (mv < v[i + 1])
      return (uint8_t)(p[i] + (uint32_t)(mv - v[i]) * (p[i + 1] - p[i]) / (v[i + 1] - v[i]));
  return 100;
}

void batteryInit() {
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);   // ~0-3.1V full scale; BAT/3 tops out ~1.4V
}

void batteryPoll() {
  uint32_t acc = 0; const int N = 8;
  for (int i = 0; i < N; i++) acc += analogReadMilliVolts(BATTERY_ADC_PIN);   // mV at GPIO20
  const uint32_t pack = (acc / N) * BATTERY_DIVIDER;                          // -> pack mV
  gMv     = gPrimed ? (uint16_t)((gMv * 3 + pack) / 4) : (uint16_t)pack;      // EMA over ~4 polls
  gPrimed = true;
  gPct    = pctFromMv(gMv);
  gAt     = millis();
}

bool batteryAvailable() { return true; }

bool batteryRead(uint16_t& mv, uint8_t& pct, uint32_t& ageMs) {
  if (!gPrimed) return false;
  mv = gMv; pct = gPct; ageMs = millis() - gAt;
  return true;
}

#else   // no battery sense on this board -> no-ops

void batteryInit() {}
void batteryPoll() {}
bool batteryAvailable() { return false; }
bool batteryRead(uint16_t&, uint8_t&, uint32_t&) { return false; }

#endif
