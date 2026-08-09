#include "common.h"
#include "battery.h"

#if BOARD_HAS_BATTERY

#define BATT_EXT_HI 4160     // rail >= this: pinned at the charger's plateau (on USB; full or absent)
#define BATT_EXT_LO 4120     // rail <= this: clearly a discharging cell -> a % is meaningful

static uint16_t gMv       = 0;    // smoothed BAT-rail voltage (mV)
static uint8_t  gPct      = 0;
static bool     gExternal = true; // rail pinned at the charge plateau (assume so until proven a cell)
static uint32_t gAt       = 0;    // millis() of last sample
static bool     gPrimed   = false;

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
  // On USB the ETA6098 holds BAT at its ~4.2V charge target whether a cell is full or ABSENT, so
  // a plateau reading means "external power, % not meaningful". Hysteresis avoids boundary flicker.
  if      (gMv >= BATT_EXT_HI) gExternal = true;
  else if (gMv <= BATT_EXT_LO) gExternal = false;
  gAt     = millis();
}

bool batteryAvailable() { return true; }

bool batteryRead(uint16_t& mv, uint8_t& pct, bool& external, uint32_t& ageMs) {
  if (!gPrimed) return false;
  mv = gMv; pct = gPct; external = gExternal; ageMs = millis() - gAt;
  return true;
}

#else   // no battery sense on this board -> no-ops

void batteryInit() {}
void batteryPoll() {}
bool batteryAvailable() { return false; }
bool batteryRead(uint16_t&, uint8_t&, bool&, uint32_t&) { return false; }

#endif
