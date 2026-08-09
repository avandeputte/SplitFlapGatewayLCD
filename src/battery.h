// battery.h -- single-cell Li-ion battery voltage/level sense (7B only).
//
// The board's ETA6098 switch-mode charger has no I2C bus, and its STAT (charge-status) pin is
// not routed to the P4 -- so charge STATE (charging/full) is NOT readable in firmware. Battery
// VOLTAGE is: a resistor divider (BAT -> R92 200K / R93 100K, ÷3) drives GPIO20, a P4 ADC pin.
// batteryPoll() ADC-samples it on taskRTC and caches a smoothed pack voltage + a rough %.
// Compiles to no-ops on boards without BOARD_HAS_BATTERY (e.g. the PoE 10.1").
#pragma once
#include <stdint.h>

void batteryInit();                                              // configure the ADC pin (setup())
void batteryPoll();                                              // one measurement -> cache
bool batteryAvailable();                                         // BOARD_HAS_BATTERY
bool batteryRead(uint16_t& mv, uint8_t& pct, uint32_t& ageMs);   // last reading; mv = pack millivolts
