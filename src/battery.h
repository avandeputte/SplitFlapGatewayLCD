// battery.h -- single-cell Li-ion battery voltage/level sense (7B only).
//
// The board's ETA6098 switch-mode charger has no I2C bus, and its STAT (charge-status) pin is
// not routed to the P4 -- so charge STATE (charging/full) is NOT readable in firmware. Battery
// VOLTAGE is: a resistor divider (BAT -> R92 200K / R93 100K, ÷3) drives GPIO20, a P4 ADC pin.
// batteryPoll() ADC-samples it on taskRTC and caches a smoothed pack voltage + a rough %.
// Compiles to no-ops on boards without BOARD_HAS_BATTERY (e.g. the PoE 10.1").
#pragma once
#include <stdint.h>

void batteryInit();          // configure the ADC pin (setup())
void batteryPoll();          // one measurement -> cache
bool batteryAvailable();     // BOARD_HAS_BATTERY (the board has a battery-voltage sense)
// Last reading. mv = BAT-rail millivolts (always valid). external = the rail is pinned at the
// charger's ~4.2V plateau -> on USB power, a cell that is full OR absent (indistinguishable),
// so pct is NOT meaningful. pct = rough % off the resting Li-ion curve; use only when !external.
bool batteryRead(uint16_t& mv, uint8_t& pct, bool& external, uint32_t& ageMs);
