#ifndef MPGW_COMMON_H
#define MPGW_COMMON_H

/*
 * LCD Gateway
 * Firmware for the Waveshare ESP32-P4-WIFI6-POE-ETH board driving a 10.1"
 * MIPI-DSI IPS touch panel (JD9365, 1280x800 landscape). A fork of the Matrix
 * Portal Gateway (ESP32-S3 + HUB75 LED matrix); that lineage traces back through
 * the Adafruit MatrixPortal S3 to the Split-Flap Gateway.
 *
 * It keeps the entire gateway -- web UI, REST API, OTA, command monitor -- and
 * drives a panel full of *virtual* split-flap modules. Nothing is wired to a real
 * reel: every module is emulated in firmware and drawn as a flap card on the LCD.
 *
 * The emulation is at the PROTOCOL level, not the API level. Commands are framed,
 * sanitized and "transmitted" exactly as before; the virtual modules parse those
 * bytes and reply with real protocol frames, staggered the way the physical
 * modules' replies would be. The gateway cannot tell the difference, so the
 * companion app keeps working unchanged.
 *
 * Copyright (c) 2026 Alex Van de Putte
 *
 * Licensed under the Creative Commons Attribution-NonCommercial-ShareAlike 4.0
 * International License (CC BY-NC-SA 4.0):
 * https://creativecommons.org/licenses/by-nc-sa/4.0/
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 *
 * Split-flap module hardware and the initial protocol by Adam G Makes
 * (YouTube: https://www.youtube.com/@AdamGMakes).
 *
 * Emulated protocol (module firmware v31, backward-compatible to v6):
 *   Frame format: m<ADDR><CMD>[data]\n
 *   Address:      decimal (zero-padded 2-digit v6 style, or variable-length v7+)
 *                 broadcast: m** (v6) or m* (v7+), optional range m*v<lo>-<hi>
 *                 by serial: mX...
 *   Commands the virtual modules ACT ON (vmDispatch):
 *                 m<id>-<char>    display character
 *                 m<id>+<n>       display by flap index
 *                 m<id>h          home
 *   Nothing replies. The physical protocol's query commands ('v' version,
 *   'A' all-fields) and by-serial addressing were removed in v1.24 -- no
 *   client ever sent them, and the wall self-describes through
 *   /api/capabilities and /api/display/state. The calibration/dump grammar
 *   is likewise unmodelled. All such frames pass the sanitizer untrimmed and
 *   the virtual modules silently ignore them, like any unknown command.
 *
 * Board: Waveshare ESP32-P4-WIFI6-POE-ETH (dual RISC-V @400MHz, 32MB flash,
 *        32MB octal PSRAM @200MHz, WiFi6/BLE via an ESP32-C6 over SDIO, IP101
 *        Ethernet + PoE, MIPI-DSI, GT911 touch, ES8311 codec + onboard mic, TF
 *        slot). No RTC chip and no IMU (NTP clock; touch replaces tap gestures).
 * Libraries: for the panel, the vendored JD9365 esp_lcd driver (see panel.cpp);
 *            ArduinoJson; AnimatedGIF.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <time.h>
#include <Update.h>
#include <FFat.h>
#include <ESPmDNS.h>
#include <esp_mac.h>

/* ---- This board's unique id -------------------------------------------------------
   Derived from the NIC-specific half of the MAC (bytes 3..5) -- the only part that
   actually differs between two boards.

   NOT ESP.getEfuseMac() masked, which is the trap this exists to close.
   esp_efuse_mac_get_default() writes the six MAC bytes in NETWORK order, and Arduino's
   EspClass reads that same buffer straight back as a LITTLE-endian uint64 -- so the LOW
   bytes of the value it returns are the OUI (48:27:e2), which is identical on every
   Espressif chip ever made. Masking the low 24 or 32 bits therefore hands EVERY board
   the same "unique" id.

   That is not theoretical. Two of these gateways on one LAN both derived the hostname
   splitflap-gw-e22748 and both connected to MQTT as splitflap-20E22748 -- and a broker
   evicts the client already holding a duplicate id, so the pair knocked each other
   offline in a loop, forever.                                                          */
static inline uint32_t boardId24() {          // 6 hex digits -- hostname suffix
  uint8_t m[6] = {0};
  esp_efuse_mac_get_default(m);
  return ((uint32_t)m[3] << 16) | ((uint32_t)m[4] << 8) | (uint32_t)m[5];
}

/* ============================================================================
 *  BOARD / BUILD CONFIGURATION
 *  ----------------------------------------------------------------------------
 *  Everything needed to retarget this firmware to a different board, panel, or
 *  default setup lives in this single block.
 *
 *  Default config is for the Waveshare ESP32-P4 + 10.1" DSI panel.
 * ==========================================================================*/

// ---- Display: MIPI-DSI (2-lane) on the ESP32-P4 -- no parallel pin map, the DSI PHY
// has dedicated pins. What the board DOES route on ordinary GPIO:
#define DSI_RESET_PIN   -1       // panel reset: TODO confirm from the board schematic (-1 = none)
#define LCD_BL_I2C_ADDR 0x45     // backlight/power controller on the display FPC (regs 0x95/0x96)


/* ---- Other on-board hardware (Waveshare ESP32-P4) ---- */
#define I2C_SDA_PIN     7        // shared bus: display backlight ctrl (0x45), GT9xx touch,
#define I2C_SCL_PIN     8        // ES8311 codec control -- Waveshare P4 demo wiring

/* ---- Time ----
   No RTC chip on this board: the system clock is invalid until the first NTP sync,
   and rtcEpochNow() returns 0 until then (every consumer handles it). See rtc.cpp. */

/* ---- Firmware identity ---- */
// ONE honest version: FW_VERSION is reported as "version"/"fwVersion" by GET /api/config
// and as "fw" in capabilities. Clients key product-specific behaviour on "product" and on
// capability tokens, never on version heuristics (the old API_VERSION="3.1.0" masquerade
// for the physical-gateway companion gate was removed in v3.12).
#define FW_VERSION           "0.3.4"    // this product's version (UI + boot log)
#define PRODUCT_NAME         "LCD Gateway"

/* ---- Network / service defaults (overridable at runtime via Settings) ---- */
/* WiFi credentials are intentionally BLANK. A freshly flashed board therefore comes
   up on its SoftAP setup page ("Split-Flap-GW") so you enter the network once from the
   browser; after that NVS holds it and these are never consulted again.

   For a personal bench build you may temporarily hard-code an SSID/password here to
   skip the setup page -- but they get compiled into the firmware image as plain
   strings (readable with `strings firmware.bin`), so NEVER commit real credentials or
   ship a binary built with them. Blank them again before publishing. */
#define DEFAULT_WIFI_SSID    ""
#define DEFAULT_WIFI_PASS    ""

// Hostname: mDNS name, DHCP name and the fallback-AP SSID all come
// from cfgHostname(). The default carries the low 24 bits of the eFuse MAC, because
// several gateways on one LAN cannot all answer to "splitflap-gw.local". Override it
// from Settings; blank means "derive it".
#define HOSTNAME_MAX         32
#define HOSTNAME_PREFIX      "splitflap-gw"
#define DEFAULT_AP_PASS      "12345678"       // SoftAP password (>= 8 chars)
#define DEFAULT_NTP_SERVER   "pool.ntp.org"
#define NTP_TIMEOUT_MS       8000UL

/* ---- Panel defaults ----
   The panel geometry is fixed (the 800x1280 DSI panel mounted landscape, 1280x800).
   The grid is the emulated wall -- one virtual split-flap module per cell, IDs
   row-major from 0 -- and is runtime-configurable (Settings -> Module Wall, applied
   on reboot). The default 15x5 gives 80x160 px flap cards; anything from 10x1 to
   32x10 works, cards always 1:FLAP_ASPECT, wall centred (see dispPlan in display.cpp). */
#define DEFAULT_PANEL_W      1280   // landscape: the 800x1280 portrait panel mounted wide
#define DEFAULT_PANEL_H      800
#define DEFAULT_GRID_COLS    15     // virtual modules across (80x160 px flap cards)
#define DEFAULT_GRID_ROWS    5      // virtual modules down; any 10x1..32x10 grid works
#define FLAP_ASPECT          2      // card height is at most this x width (see dispPlan)
#define DEFAULT_PANEL_BGR    false  // true if the panel is wired BGR (see panelSetColourOrder)
#define DEFAULT_BRIGHTNESS   160    // 1..255, scales every colour before output
// A real module flips a handful of flaps per second, not fifty. This also sets the
// repaint rate: one flap is two half-steps, so the panel redraws at 2000/flapMs Hz
// while a reel is turning (DISP_MIN_FRAME_MS in display.cpp caps it regardless).
#define DEFAULT_FLAP_MS      60     // ms per flap step -- the reel's speed
// Draw a geometry test pattern for four seconds at boot, before any flap content. It is
// on by default: it costs four seconds and it tells you which layer is wrong the moment
// the wall looks scrambled, without a rebuild.
//   * corner colours not R/G/B/W clockwise from TL -> RGB order / rotation (panelBGR,
//     PANEL_ROT_180 in panel.cpp)
//   * pattern skewed, stretched or offset          -> DSI timing / geometry
//   * pattern perfect but flaps garbled            -> content, not the panel
#define PANEL_BOOT_TEST      1


// Circuit breaker for the large canvas ops (animation + QOI upload, framebuffer readback). If free
// internal heap is already below this when one arrives, refuse it with 507 rather than pile its
// transient (PSRAM alloc + panel takeover, or a ~48 KB response stream) onto a stressed heap and
// risk loop()'s 20 KB reboot floor. 2x the floor: a genuinely-low signal, not a normal dip.
// (Conservative on the P4, where the framebuffer is in PSRAM and internal heap stays
// healthy -- this rarely engages; kept as a floor for the network-buffer transient.)
#define CANVAS_MIN_UPLOAD_HEAP  (40u * 1024u)

#define PANEL_MAX_W          1280
#define EFFECT_RENDER_SCALE  5     // effects draw at 1/5 (256x160); the PPA scales up (panel.h)

/* ---- Flip animation ----
   Changing the displayed flap cascades forward through the reel, which is what
   makes the panel read as a split-flap. It is a rendering effect, not a mechanism.

   FLAP_ANIM_MAX bounds one change to 64 flips (64 * DEFAULT_FLAP_MS ~= 3.8 s for
   the longest cascade -- a physical 64-flap reel's full revolution, kept as the
   cap even though this reel has 237 flaps). A longer jump starts its walk flapMax
   flaps short of the destination. Set flapMax to 1 for an instant cut. */
#define DEFAULT_FLAP_MAX     64     // flips drawn for one character change
#define FLAP_ANIM_MAX        64     // hard ceiling on flapMax


/* ---- Flap set sizing ----
   A physical reel has 64 leaves because it is a physical object. These modules are DRAWN,
   so there is nothing to ration: the reel carries 237 flaps -- every printable CP1252
   glyph, the seven colour flaps, every lowercase letter, and the pictographs. It is BUILT
   AT BOOT (reelBuild in reel.h) rather than typed out, and it is not stored per module and
   not configurable -- a reel that can already draw everything has nothing left to
   reconfigure, so the physical gateway's 'N' command and flap-set editor are gone.

   The SECTION ORDER is the contract (see reel.h for the full story): the 156 uppercase
   glyph flaps and the 7 colour flaps (indices 0..162) come first, byte-compatible with
   the classic reel; the lowercase and pictograph flaps live PAST them, reachable only by
   index. On the one-byte legacy path lowercase still folds to uppercase -- it must,
   because the bytes r o y g b p w mean colours there -- while the index-addressed API
   (/api/display/cells, /api/flap/index) reaches every flap without folding.

   Flap 0 is blank (the home position).

   ONE COMPATIBILITY NOTE, deliberately accepted: the 'A' reply reports flapCount as
   SF_LEGACY_FLAPS (163), and splitflap-os (server/app.py) rejects any count outside
   1..64, so it will refuse this reel and fall back to its own map. That costs nothing
   here -- it addresses flaps BY CHARACTER ('-'), never by index, so all of its text
   still displays exactly as before. The gateway's own UI/REST/MQTT are unaffected. */
#include "reel.h"          // SF_MAX_FLAPS / SF_COLOUR_FLAPS / SF_CHAR_FLAPS, and the reel
                           // itself -- Arduino-free, so tools/reel_test.cpp compiles the
                           // very same code rather than a copy of it.
#define SF_MAX_TEXT          256   // longest text sfSendText will lay across modules

/* ---- Buffer / queue sizes ----
   TX_MAX_BYTES (512) is kept generous for raw frame sends. */
#define MSG_RING_SIZE        64    // command log: number of entries retained
#define TX_MAX_BYTES         512   // max bytes frameSend will transmit in one frame

/* ---- Housekeeping cadences ---- */
// Longer than a companion heartbeat (~30 s) ON PURPOSE. Each change RESTARTS this
// clock, so two companions flipping the URL between them never hold still long enough
// to be written -- which is the point. A single, real change persists after two quiet
// minutes.
#define COMPANION_SAVE_DEBOUNCE_MS 120000UL   // companion URL: persist once it settles

/* ---- Persisted files (FFat) ----
   Only the companion settings blob below. The virtual modules persist NOTHING:
   every field is deterministic from the configured grid (id = wall slot, serial
   derived from the MAC, reel homed at boot), so the physical gateway's
   /modules.dat registry and this product's former /vmods.dat are both gone --
   vmInit() deletes a leftover file from older firmware. */

/* ---- Companion settings blob (FFat) ---- */
#define COMPANION_FILE       "/compset.gz"
#define COMPANION_TMP        "/compset.tmp"
#define COMPANION_MAX_BYTES  (64UL * 1024UL)

/* ---- Companion tab advertisement (v1.4) --------------------------------------------
   The two apps tell each other which tabs they have, so each nav can deep-link the
   other's screens instead of hard-coding a list that goes stale. The companion POSTs
   its `tabs` to /api/companion; the response always carries this firmware's `gwTabs`.
   Either side may say nothing (an older peer), and the other then falls back to its
   own built-in list. */
#define COMPANION_TABS_MAX     384
#define COMPANION_TABS_MAX_N   10    // max tabs accepted from a companion
#define COMPANION_TAB_ID_MAX   24    // max chars of one tab's id (the URL hash)
#define COMPANION_TAB_LBL_MAX  24    // max chars of one tab's label

/* ==========================================================================*/
#define DBG(...) do { if (gSerialDebug) printf(__VA_ARGS__); } while(0)

/* ============================================================================
 *  SHARED INFRASTRUCTURE  (cross-cutting; defined once in globals.cpp)
 * ==========================================================================*/
extern volatile bool gSerialDebug;
extern volatile bool gQuietTime;
extern char gCompanionStatus[80];
extern char gCompanionTabs[COMPANION_TABS_MAX];   // the companion's tabs, already serialised JSON
extern volatile unsigned long gCompanionSeenMs;
// The companion URL is persisted on a DEBOUNCE, not on every change. Two companions
// pointed at the same gateway will each re-register their own URL on their heartbeat,
// so cfg.companionUrl flips back and forth -- and saving on every change turned that
// into an NVS write every ~30 s, forever. Observed in the wild. The URL is applied to
// RAM immediately (the UI and the companion tabs are live at once); only the flash
// write waits for the value to hold still. A contested URL therefore never reaches
// flash at all, which is the right answer: nothing durable should be written for a
// value two clients are still arguing over. A companion re-registers within a
// heartbeat of any reboot, so nothing is lost by not persisting it.
extern volatile bool          gCompanionUrlDirty;
extern volatile unsigned long gCompanionUrlDirtyMs;
extern volatile bool gOtaInProgress;
// Raw-canvas mode: HTTP owns the panel and the reel renderer stands down (see /api/canvas).
extern volatile bool gCanvasMode;
extern volatile bool gCanvasStreamKill;   // wall takeover -> close any lingering canvas stream
// Acknowledgement from taskDisplay that it has parked for canvas/OTA; the take-over waits on it.
extern volatile bool gDispParked;
extern volatile bool gOtaRebootPending;
extern bool gApActive;
extern volatile bool gEthUp;   // Ethernet has an IP -> taskNetwork powers WiFi down (wired preferred)
extern SemaphoreHandle_t timeMutex;
extern StaticSemaphore_t timeMutexBuf;
extern volatile unsigned long wdgFramesMs;
extern volatile unsigned long wdgNetMs;
extern volatile unsigned long wdgWebMs;
extern volatile unsigned long wdgDispMs;
extern TaskHandle_t hTaskRTC, hTaskFrames, hTaskWeb, hTaskNet, hTaskDisp;
extern bool ntpSynced;
extern unsigned long staDownSince;

#endif // MPGW_COMMON_H
