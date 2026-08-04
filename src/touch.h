// touch.h -- the 10.1" panel's capacitive touch (GT911), LCD Gateway.
//
// The Waveshare panel carries a GT911 controller on the shared I2C bus (0x5D, or
// 0x14 on the backup strap); INT and RST are not wired, so it is polled. Same I2C
// discipline as the sensors (the bus has NO lock): touchInit() probes at boot while
// setup() is single-threaded, and touchTick() -- the ONLY runtime I2C here -- runs on
// taskRTC. touchPoll() is I2C-free (it reads a published flag) so any task can drain
// events.
//
// This is the LCD's answer to the Matrix boards' IMU taps: a physical way to dismiss
// a timer or alarm without a browser, and a `touch` gesture the companion can bind.
// A tap is a press-and-release; two within the pairing window are a double.

#pragma once
#include <stdint.h>

void touchInit();                                   // probe + configure (setup() only)
bool touchAvailable();                              // GT911 found
void touchTick();                                   // poll the panel (taskRTC only, ~100 ms)
// One event per completed tap gesture; count 1 = single, 2 = double. Any task.
bool touchPoll(uint8_t* countOut, uint32_t* seqOut);
uint32_t touchTotal();                              // tap events since boot (diagnostics)
// Last touch point in LOGICAL landscape pixels, and whether a finger is down right
// now -- for the dashboard/telemetry. Reset-on-read is not needed; these just mirror.
bool touchPoint(uint16_t* x, uint16_t* y);
