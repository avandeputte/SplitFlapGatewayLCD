# Board Support Plan — LCD Gateway on multiple ESP32-P4 boards

**Goal:** one firmware tree, multiple Waveshare ESP32-P4 display boards, selected at build
time. The compute core — ESP32-P4 + ESP32-C6 (WiFi 6 over SDIO), PSRAM, PPA, 2D-DMA — is
**identical** across the boards; only the panel, networking, and a few peripherals differ.

## Boards

| | **10.1″ (shipping)** | **7B (target)** |
|---|---|---|
| Board | Waveshare ESP32-P4-WIFI6-POE-ETH + 10.1″ DSI | Waveshare ESP32-P4-WIFI6-Touch-LCD-7B |
| Panel | JD9365, **800×1280 portrait**, vendored driver | **EK79007, 1024×600 native landscape**, `espressif/esp_lcd_ek79007` component |
| Rotation | **mandatory 90° PPA rotate** | **none** (native landscape) |
| Network | Ethernet (IP101 PHY) + PoE, WiFi via C6 | **WiFi-only** |
| Audio | ES8311 + single SMD mic | ES8311 + **ES7210** (dual mic, echo-cancel) + speaker |
| RTC | none (NTP-only) | **RTC + 1220 battery holder** |
| Touch | GT911 (I2C 7/8) | GT-series 5-pt (likely same driver/bus) |
| SD | SDMMC slot 0 (slot 1 = C6 radio) | SDIO TF (slot 0 expected, same reason) |
| Camera | — | MIPI-CSI (OV5647, unused here) |
| Power | PoE / USB-C | USB-C + MX1.25 battery |

Everything above the panel — the virtual-flap wall, canvas, effects, ticker, web/API,
companion — is resolution-driven already (`dispPlan` computes cards from `panelW/H`, the
renderer picks fonts by cell size), so it adapts to 1024×600 with mostly validation work.

## Architecture

1. **Build-time board select** — one PlatformIO env per board, each with a `-DBOARD_*` macro.
2. **Board-profile headers** under `src/boards/`, selected by `src/board.h`, centralizing
   every board-specific fact (panel driver + geometry + rotation, DSI pins/timing, I2C pins,
   backlight method, capability flags). `common.h` and `panel.cpp` read from the profile.
3. **Panel HAL** — drawing primitives stay board-agnostic (they work on the logical
   framebuffer); only *init* (driver + timing) and *present* (rotate-and-swap vs direct copy)
   are board-parameterized.
4. **Feature gating + identity** — `#if BOARD_HAS_ETH` / `BOARD_HAS_RTC`, an audio-codec
   branch, and a `board` field in `/api/config` + the capabilities descriptor.

## The hard part: optional rotation

`panelShow()` today **always** PPA-rotates logical-landscape → native-portrait, and the
pixel-effects upscale piggybacks that same pass. The 7B is native landscape → **direct
present**. Plan: make the present's compose op a profile-selected `rotate` (10.1″) or `blit`
(7B) via the PPA/2D-DMA, preserving the draw/live double-buffer + swap and every present-model
invariant (epochs, dirty-cell wall, blip, overlay hook). Net for the 7B: ~40% fewer pixels
(614K vs 1024K) and no rotate → the current ~53 ms rotate floor largely disappears.

## Phases

- **Phase 0 — Confirm 7B specifics** *(needs the board + Waveshare ESP-IDF demo/schematic)*:
  EK79007 DPI timing & lane count, touch controller + I2C address, backlight method, SD pins,
  RTC part number, ES7210 wiring. *Blocks Phase 2/3.*
- **Phase 1 — Board-profile refactor** *(no 7B required; THIS COMMIT)*: extract the 10.1″
  assumptions into `src/boards/` behind a profile layer; add the board-select macro; scaffold
  the 7B profile with best-known values + TODOs. **10.1″ behavior unchanged.**
- **Phase 2 — 7B panel bring-up**: add `esp_lcd_ek79007` + the 7B profile + the direct-present
  path; boot test pattern → flap wall on the 7B; verify `LCD_DSI_ISR_CACHE_SAFE` holds.
- **Phase 3 — 7B peripherals**: WiFi-only networking (gate ETH out), touch, SD, backlight →
  fully functional gateway.
- **Phase 4 — Board-enabled features** *(optional/deferrable)*: RTC battery clock; ES7210
  dual-mic capture for the audio effects.
- **Phase 5 — Identity + validation**: `board`/panel reporting; geometry & font legibility at
  1024×600; both envs build from one tree.

## Risks / unknowns

- The **Phase-0 confirmations** above — especially EK79007 DPI timing and touch/backlight wiring.
  Wrong timing = no image or a skewed one.
- **`LCD_DSI_ISR_CACHE_SAFE`** was load-bearing for the RAM-less JD9365 (garbage on flash
  writes). The EK79007 is the same "no panel RAM, ISR re-arms scanout" architecture, so it
  almost certainly needs the same option — verify early.
- **Rotation-removal** is invasive to a mature present model; keep the 10.1″ path literally
  unchanged and add the direct path beside it.

## Status

- **Phase 1: in progress** (this commit). Phases 0 and 2–5 pending; Phases 2+ need the 7B in hand.

## References

- Waveshare ESP32-P4-WIFI6-Touch-LCD-7B — product page, wiki, and documentation platform.
- EK79007 panel driver: `idf.py add-dependency "espressif/esp_lcd_ek79007"` (ESP Component Registry).
