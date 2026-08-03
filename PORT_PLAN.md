# LCD Gateway — ESP32-P4 / DSI port plan

Forked from **MatrixPortalGateway v3.16.0**. Same product family, new surface: a
Waveshare **ESP32-P4-WIFI6-POE-ETH** driving the Waveshare **10.1" DSI touch
display** (800×1280 portrait, mounted landscape 1280×800).

## Hardware facts (researched 2026-08-03)

| Subsystem | This board | vs. Matrix boards |
|---|---|---|
| SoC | ESP32-P4 rev 3.x, 2× RISC-V @ 400 MHz | ESP32-S3 |
| Memory | 32 MB NOR flash, 32 MB in-package PSRAM | 32 MB flash, 16 MB PSRAM |
| Radio | none on-die — **ESP32-C6 over SDIO** (esp_hosted; Arduino WiFi API remoted) | native WiFi |
| Ethernet | **IP101 PHY + RJ45 + PoE** — new capability | none |
| Display | MIPI-DSI 2-lane → **JD9365** controller, 800×1280 IPS @ 60 Hz | HUB75 LED matrix |
| Touch | GT-series capacitive, 10-point, I2C — new capability | none |
| Audio | ES8311 codec + NS4150B amp + speaker header + **onboard SMD mic** | ES8311 spkr + ES7210 quad mic |
| Backlight | I2C controller **0x45** on the display FPC: reg 0x95 power, **0x96 = brightness 0–255** | OE duty cycle |
| I2C | SDA=7 SCL=8 (backlight, touch, codec ctrl) | 47/48 |
| SD | SDIO 3.0 TF slot (pins TBD from schematic) | 1-bit CLK=1 CMD=44 D0=17 |
| RTC chip | **none** — NTP-only (PCF85063 probe fails gracefully) | PCF85063 |
| IMU | **none** — no tap gestures; touch replaces them | QMI8658 |
| Console | USB-Serial-JTAG | USB CDC |

## What already works (compiles, honest state)

- **Toolchain**: pioarduino `esp32-p4_r3` board, Arduino core 3.3.9 — whole tree builds.
- **panel.cpp fully rewritten** for DSI: linear RGB565 double buffer in PSRAM, JD9365
  bring-up via the vendored Waveshare driver (`esp_lcd_jd9365_10_1.c/.h`, Apache-2.0,
  i2c_bus dependency removed), DPI scanout, **PPA hardware 90° rotate** for the
  landscape mount (CPU fallback included), real backlight brightness, same panel.h
  contract — clip/blend/layers/blit/scroll/blip all carried over. HUB75 bitplane/GDMA
  machinery and its watchdog: gone.
- Everything above the panel (web, canvas/ops JSON+binary, effects, timer/alarms,
  clap gestures, SD backup, config export/import) compiled untouched.

## Bench findings (2026-08-03, first hardware session)

- Chip is **ECO2 (pre-rev-3.00)** silicon → board def `esp32-p4` (ES), not `_r3`.
- Boots to `[Boot] Ready`: 32 MB PSRAM + flash detected, all absent peripherals
  degrade gracefully, ES8311 ACKs at 0x18, web server serves the full API.
- "4 MB flash ceiling" was a false alarm — it's the app-slot size from the
  partition table, same as the Matrix boards.
- **C6 hosted link WORKS** (API served over the fallback AP) but the shipped slave
  firmware predates the version RPC and **fails WPA auth against S4** → needs the
  2.12.8 slave image → `POST /api/system/c6ota` added (raw-body, flashes the C6
  through the P4). macOS won't hold an internet-less AP long enough for the push —
  the update goes over **Ethernet** once a cable is in.
- Display: the 0x45 backlight/power controller does not ACK → the panel had no
  power (separate power lead required beyond the DSI FPC). panelBegin now
  preflights 0x45 and runs headless instead of hanging in the JD9365 ID read.
- Ethernet wired in (IP101, IDF-default P4 RMII pins) — silent init, awaiting link.

## Milestone 1 — first pixels (needs the hardware on the desk)

1. Flash over USB; watch USB-Serial-JTAG console.
2. **Panel lights?** JD9365 init + backlight sequence are exactly Waveshare's; the
   open risks are the DSI reset pin (`DSI_RESET_PIN` in common.h, currently -1 —
   confirm from schematic) and LDO channel 3 assumption (Waveshare demos use it).
3. **Rotation direction**: PANEL_ROT_180 flip if the mount reads upside-down.
4. **WiFi via C6**: Arduino WiFi should "just work" through esp_hosted **if** the
   board wires the C6 on the default SDIO pins — verify; else sdkconfig pin override.
5. **Flash size**: build currently reports a 4 MB ceiling — board_upload override
   didn't take; revisit (32 MB part, partitions-32MB.csv) before OTA matters.
6. Wall renders at 1280×800 with existing fonts (small glyphs in huge cells — fine
   for bring-up; big-flap font generation is Milestone 3).

## Milestone 2 — the network the board deserves

- **Ethernet** (ETH library, IP101, PoE) alongside WiFi; advertise which is up.
- SD pins from schematic → card + event log + backup work as on Matrix.
- ES8311 speaker on the P4's I2S pins; mic capture path (single SMD mic — the clap
  detector's not-bass gate needs re-calibration for it, ES7210 assumptions removed).

## Milestone 3 — what only this surface can do

- **Touch** (GT9xx driver): tap-to-dismiss timers/alarms (replacing IMU taps),
  on-screen buttons, maybe swipe between wall/canvas modes. New `touch` capability.
- Big-flap font: crisp 10×20-per-cell is comical at 85×266 px cells; generate a
  proper large AA glyph set (tools/genfont.py at a bigger size, or runtime scaling).
- Panel-native niceties: real 60 fps effects (the P4 has the headroom), maybe RGB888.

## Deliberate carry-overs to revisit

- `fbPsram`, `panelBitDepth`, BGR toggle, GDMA-era settings still exist in config/UI —
  harmless but meaningless here; strip in the first cleanup pass (no-legacy rule).
- `taps` capability code is present but the QMI8658 probe fails → token never
  advertised. Remove with the touch work.
- openapi/wiki still describe the Matrix product; rewrite once bring-up proves real.
