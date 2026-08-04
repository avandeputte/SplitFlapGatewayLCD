# LCD Gateway

> ### 📖 [SplitFlap Wiki — the comprehensive documentation](https://github.com/avandeputte/SplitFlapGateway/wiki)
> Quick start · choosing a configuration · provisioning & calibration · the SplitFlap and
> Matrix Gateways · the companion and its apps · APIs and wire protocols — the whole
> ecosystem, documented in one place.


A split-flap display with no split flaps.

This is a fork of the [Matrix Portal Gateway](../MatrixPortalGateway/1.0) firmware, ported to a
**Waveshare ESP32-P4-WIFI6-POE-ETH board** driving a **10.1" MIPI-DSI touch LCD** (JD9365,
1280×800 landscape). That lineage traces back through the ESP32-S3 + HUB75 Matrix boards to
the [Split-Flap Gateway](../../SplitFlapGateway/3.1). The physical gateway's serial
transceiver is gone. In its place is a wall of *virtual* split-flap modules, each one
receiving the same protocol frames a real module would, acting on the display commands
(`-`, `+`, `h`), and rendering itself as a flapping character card on the panel.

Everything above the protocol seam is unchanged: the web UI, the REST API, OTA, the
command log. The [companion app](../../SplitFlapGatewayCompanion) drives it without
modification and cannot tell the difference. This board adds a **capacitive touchscreen**
(double-tap to dismiss timers/alarms) and **Ethernet + PoE**; it has no RTC chip (NTP clock)
and no IMU (touch replaces tap gestures).

```
     ┌──────────────────────────────────────────────┐
     │  web UI · REST API · OTA · command log       │   carried over
     ├──────────────────────────────────────────────┤
     │  frameSend()  framing · sanitization · Quiet  │   unchanged
     ├──────────────────────────────────────────────┤
     │  vmodule      virtual split-flap modules      │
     ├──────────────────────────────────────────────┤
     │  display      flap-card renderer              │
     ├──────────────────────────────────────────────┤
     │  panel        ESP32-P4 MIPI-DSI driver + PPA  │   (vendored JD9365 esp_lcd driver)
     └──────────────────────────────────────────────┘
```

Per-release history — features, breaking changes, certification results — lives in
[RELEASE_NOTES.md](RELEASE_NOTES.md).

---

## Table of Contents

- [What it does](#what-it-does)
- [Beyond the flaps: the pixel surface](#beyond-the-flaps-the-pixel-surface)
- [The reel](#the-reel)
  - [Why `m5-r` shows red and not the letter r](#why-m5-r-shows-red-and-not-the-letter-r)
- [What is and is not emulated](#what-is-and-is-not-emulated)
- [The panel](#the-panel)
- [The flip](#the-flip)
- [Configuration](#configuration)
- [Language](#language)
- [Compatibility](#compatibility)
- [Repository contents](#repository-contents)
- [Building](#building)
- [Known limitations](#known-limitations)

---

## What it does

On boot the firmware creates one virtual split-flap module per cell of the module wall
(15 × 5 = **75 modules** by default), with IDs `0`…`74` fixed by wall position. There is no
discovery, because there is nothing to discover: every module exists by construction, and the
array of them *is* the state of the wall. Nobody has to ask, and (since v1.24) nobody *can*
ask over the protocol — the wall self-describes through `/api/display/state`,
`/api/flap/modules` and `/api/capabilities` instead.

From then on, everything works. Send `m5-A` and module 5's reel flips forward until it lands
on `A`. Send text from the Display tab and it cascades across the wall. Every command the
gateway receives is recorded in the command log (`GET /api/log`).

The difference is that nothing is moving. The modules are software, and the panel is where
they live.

---

## Beyond the flaps: the pixel surface

Because the "wall" is really a framebuffer, the gateway exposes it directly alongside the
split-flap emulation — everything feature-detected through `GET /api/capabilities`, fully
specified in [openapi.yaml](openapi.yaml) and the
[wiki's Canvas page](https://github.com/avandeputte/SplitFlapGateway/wiki/Canvas):

- **Canvas mode** — push raw frames (`rgb888`/`rgb565`/QOI), partial rects, multi-rect
  deltas, or a JSON batch of draw ops (shapes, text in six sizes plus uploadable fonts,
  sprites); configure crossfade/wipe/slide **transitions** for full-frame presents.
- **The stream channel** (`PUT /api/canvas/stream`) — one long-lived connection carrying
  draw records back-to-back for animation-rate updates with no per-frame HTTP round trip.
- **The atlas library** — up to 16 named sprite sheets resident in PSRAM, optionally
  persisted to flash, blitted by index from draw ops or the stream.
- **On-device content** — a stored animation library (with GIF import) and boot
  animation, a scrolling ticker (exclusive or overlaid), and effects: plasma, fire,
  matrix rain, flip-o-rama, a clock, Game of Life, an oscilloscope and a spectrogram.
- **Audio-reactive visuals** — the board's onboard microphone (the ES8311 codec's ADC)
  drives a `spectrum` analyzer, an oscilloscope and spectrogram, a `soundwall` mode where
  the flap wall itself flips on beats, and an `"audio":true` option that makes
  fire/matrix/plasma react to the room. Sound is reduced to a handful of numbers on-device;
  nothing is recorded.
- **Self-describing effects** *(v3.4)* — capabilities carry `effectDefs`: every
  effect with exactly the typed, ranged, labelled params it consumes, so clients
  render effect UIs dynamically instead of hard-coding options.
- **Live events** (`GET /api/events`) — a Server-Sent Events stream carrying the display
  state the instant it changes plus a status heartbeat; the dashboard's live preview and
  its status pane ride it instead of polling.
- **Text is UTF-8 in, CP1252 flaps out** — ops/ticker text accepts the full Latin-1
  repertoire (`21° — Grüße`), transcoded on device.

The wall and the canvas hand the panel back and forth cleanly: canvas/effect/animation
modes park the flap renderer, and releasing them (`POST /api/canvas {"active":false}`)
returns whatever the reels last showed.

---

## The reel

Each virtual module carries **237 flaps**: 156 Windows-1252 glyphs, the seven colours, the
60 lowercase letters and the 14 pictographs.

A physical reel has 64 leaves because it is a physical object. These modules are *drawn*, so
there is nothing to ration — the reel simply carries **one flap for every character**, and any
Windows-1252 character you send has somewhere to land.

| Index | Contents | Reachable from |
|---|---|---|
| `0` | blank (the home position) | both |
| `0`–`155` | the CP1252 repertoire in code-point order, **minus the lowercase letters** | both |
| `156`–`162` | the seven colour flaps: `r o y g b p w` | both |
| `163`–`222` | the 60 **lowercase** letters | index only |
| `223`–`236` | 14 **pictographs**: ♥ ♦ ♣ ♠ ☺ ♪ ● ■ ⌂ ← ↑ → ↓ ☀ | index only |

The legacy sections come **first** and keep the indices they have always had, so growing the
reel can never move a flap an existing controller already addresses by number.

```
 !"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\]^_`{|}~€‚ƒ„…†‡ˆ‰Š‹ŒŽ‘’“”•–—˜™›Ÿ
¡¢£¤¥¦§¨©ª«¬®¯°±²³´µ¶·¸¹º»¼½¾¿ÀÁÂÃÄÅÆÇÈÉÊËÌÍÎÏÐÑÒÓÔÕÖ×ØÙÚÛÜÝÞß÷roygbpw
```

The reel is **shared, complete and fixed**. It is not stored per module, and it is not
configurable: a drawn reel that can already render everything has nothing left to reconfigure,
so the `N` command and the flap-set editor are gone. (The split-flap gateway keeps both — its
reels are real, and what is printed on them is a fact about the hardware.)

It is **built at boot**, not typed out — from `isFlapByte()` and `cp1252IsLower()`, which
already own *which bytes exist* and *which bytes are lowercase*. Add a glyph to the font and
the reel grows a flap for it; it cannot drift from the font or from the folding rule. The
whole thing lives in `src/reel.h`, deliberately free of Arduino, so `tools/reel_test.cpp`
compiles the *same* code rather than a copy of it.

### Two ways in, and they are not the same

The legacy wire protocol carries **one byte** per character, and it has a problem it can never
solve: **the byte for lowercase `r` already means red.** The seven colour flaps are addressed
by `r o y g b p w` — that is the protocol, not a choice. So on that path lowercase *must* fold
to uppercase, and a heart — which has no Windows-1252 byte at all — cannot be addressed by
character in **any** way.

That is not a limitation of this firmware. It is a limitation of a one-byte alphabet that
spent seven of its letters on colours.

So there are two resolvers, and the reel has flaps the old protocol simply cannot name:

| | `m<id>-<char>` (legacy) | `POST /api/display/cells` |
|---|---|---|
| `r` | the **red** flap | the **letter** r |
| `a` vs `A` | both → `A` (folded) | two different flaps |
| `♥` | impossible — no byte exists | flap 223 |
| colours | the letters `r o y g b p w` | named: `{"color":"red"}` |

**The legacy protocol is untouched.** `m5-r` is still red, `hello` still renders `HELLO`, and
every existing controller works exactly as it did. The new endpoint is a *different way in*,
not a different reel:

```jsonc
POST /api/display/cells
{
  "start": 0,
  "step_ms": 15,
  "cells": [
    {"ch": "H"}, {"ch": "e"}, {"ch": "l"}, {"ch": "l"}, {"ch": "o"},
    {"ch": "♥"},              // a pictograph, drawn in its own red
    {"color": "red"},         // a colour flap, NAMED — 'r' here would be the letter r
    {"blank": true},          // home it
    {"skip": true}            // leave that module alone
  ]
}
```

It sends `m<id>+<n>` — the index command the modules have understood all along. Every cell is
resolved **before anything is sent**: a character the reel cannot show is a `400`, not a
silent blank, because a half-written wall is worse than a rejected request.

The pictographs come from the **bundled X11 fonts**, which already carry thousands of glyphs —
not from hand-drawn bitmaps. The only exception is `☀` at 5×8, which that face genuinely
lacks, so it is drawn by hand in `tools/genfont.py`. Each one can bring its own ink: a heart
that comes out white is not a heart.

### No lowercase *on the legacy path* — and that is load-bearing

The reel is printed in capitals, like a real one, so **lowercase folds to uppercase**
(`cp1252ToUpper`). That is not a stylistic choice. The seven colour flaps are addressed by the
**lowercase letters** `r o y g b p w` — that is the protocol. Put lowercase letters on the reel
and a lookup for `r` would find the *letter*, and every colour command ever written would
quietly start printing letters instead of colours.

Folding has traps, and `charset.cpp` owns all of them in one place: `ÿ` uppercases to `Ÿ`
(`0x9F`), **not** to `ß` — a naive `-0x20` turns a y-diaeresis into an eszett. `÷` is the
division sign, not a letter, so it keeps its own flap rather than folding onto `×`. `ß` has no
uppercase in CP1252 at all, so it keeps a flap too. `œ`, `š` and `ž` uppercase nowhere near a
`0x20` offset. `µ`, `ª` and `º` look lowercase but are symbols.

`tools/reel_test.cpp` pins every one of those down against the real code.

### Why `m5-r` shows red and not the letter r

`vmFlapIndexOf()` resolves a character in a fixed order, and the order *is* the contract:

1. **the colour codes first.** `r o y g b p w` are lowercase by protocol, not letters by
   meaning. Checking them before anything else is what guarantees `r` can only ever be red.
2. **`q`** — splitflap-os's legacy alias for the double-quote flap. The classic reel had no
   lowercase, so its char map borrowed that byte. This reel carries a real `"`, so the alias
   is honoured rather than the frame being dropped.
3. **fold to uppercase** (`cp1252ToUpper`) — the reel is printed in capitals.
4. **scan the 156 glyph flaps** — never the colours, which step 1 already owns.

```
m5-r      → flap 156  the RED colour flap   (colour codes are never folded)
m5-R      → flap 50   the letter R
m5-a      → flap 33   folded to 'A'         (the reel has no lowercase letters)
m5-é      → flap 132  folded to 'É'
m5-$      → flap 4    used to be a blank — the old 64-flap reel had no '$'
m5-q      → flap 2    the '"' flap          (the legacy alias)
m5+0      → flap 0    blank
```

So `hello` still renders `HELLO`, `m5-r` still means red, and **every existing colour command
works unchanged** — while 99 characters that used to come out blank now display.

---

## What is and is not emulated

**The protocol is emulated where it is used.** The firmware acts on the three commands the
gateway still emits: display (`-`, `+`) and homing (`h`), addressed to one module or broadcast
(`m*`, and the two-star v6 form). Everything else in the physical grammar — the
calibration/dump family, the removed `v`/`A` queries, by-serial `mX…` addressing — passes
through the sanitizer untrimmed and is silently ignored: there is nothing to calibrate,
nothing to dump, and (since v1.24) nothing that answers a question the REST API does not
already answer better.

**The mechanism is not emulated at all.** There is no stepper, no Hall sensor and no EEPROM.
Nothing can be out of tune, so nothing needs tuning: the calibration, diagnostics, provisioning
and backup commands are ignored rather than faked — no reply, no state. `h` simply shows flap
0, the blank.

**The wire is not emulated either.** The physical gateway's half-duplex serial wire at
9600 baud is slow — a broadcast query across 45 modules once took seconds of staggered reply
slots. Here a frame is a function call: delivery is instant, one-way, and **nothing ever
replies**, so there is no reply timing to emulate and no collisions to worry about. The
command log (`GET /api/log`) records the outbound frames, which are the whole of the
traffic.

---

## The panel

The panel is a fixed **1280×800** MIPI-DSI IPS LCD (native 800×1280 portrait, mounted
landscape). The module grid *is* the wall, and cell size falls out of it — but a flap
**card keeps a 1:2 proportion** no matter the grid: whichever axis is the constraint sets
the card size, and a wall that does not fill the panel is centred on both axes (short walls
letterbox or pillarbox). The default is **15×5** — 75 modules, 80×160 px cards — and any grid
from **10×1 to 32×10** works.

The renderer picks the largest glyph face that fits the card. There are two families:

- **Six Helvetica flap faces** (110×154 down to 32×46 px, generated by `tools/genflapfont.py`
  from Helvetica Bold — the real split-flap typeface — with the 14 pictographs from Apple
  Symbols) cover every card width the grid range produces. Each cell draws a proper flap
  **card**: a dark card face inset in a housing gutter, a permanent split seam, and the
  mid-flip fold across the middle.
- **Seven small CP1252 bitmap faces** (`10x20` down to `5x8`, `tools/genfont.py`) serve tiny
  cells and the effects' fliporama. All carry the full 216-glyph CP1252 set with real
  diacritics plus the 14 pictographs. (`5x7`/`4x6` are absent: at those sizes the source face
  draws `À` identically to `A`, a correctness bug the generator rejects.)

Rendering runs at 60 Hz. The logical landscape frame is a linear RGB565 framebuffer in PSRAM;
`panelShow()` rotates it 90° into the DSI scanout using the P4's **PPA** (Pixel Processing
Accelerator) in hardware. Pixel effects render at a reduced 256×160 surface and the PPA
upscales them; the clock and the wall render native. The layout presets (Settings → *Module
Wall*) offer common grids; power-cycle to apply (the grid is read once at boot). The ceiling
on the emulated wall is **`VM_MAX_MODULES` = 320** (`src/vmodule.h`) — exactly a 32×10 wall.

### If every colour is wrong

If red and blue are swapped, the panel is wired **BGR** rather than RGB. Tick **Settings →
LCD Panel → "Panel is wired BGR"** — it takes effect on the next frame, no reboot. The swap
happens in `panelPixel()`, the one choke point every pixel passes through. (Rotation is the
other first-look check: if the image is upside-down, flip `PANEL_ROT_180` in `panel.cpp`.)

## The flip

Changing the displayed flap cascades forward through the reel one flap at a time. This is a
*rendering effect*, not a simulation.

`flapMax` caps one change at **64 flips** (a physical reel's full revolution, kept as the cap
even though this reel has 237 flaps); a longer jump starts its walk
`flapMax` flaps short of the destination. At the default 60 ms per flap, the longest cascade
takes about 3.8 seconds. Set `flapMax` to `1` for an instant cut.

Mid-flip, a cell shows the top half of the incoming flap above the bottom half of the outgoing
one, split by a bright seam. That is what a split-flap does, and it is why the animation reads
correctly even in an 8×10 cell.

---

## Configuration

Everything is on the Settings page and in `POST /api/config/settings`.

| Setting | Default | Applies |
|---|---|---|
| `gridRows` × `gridCols` | 5 × 15 | **on reboot** — this creates and destroys modules (up to `VM_MAX_MODULES` = 320) |
| `panelBitDepth` | 4 | **on reboot** — 1…6 RAM and EMI scale with it |
| `panelBGR` | false | next frame — see [If every colour is wrong](#if-every-colour-is-wrong) |
| `panelBright` | 160 | next frame |
| `flapMs` | 60 | next flap |
| `flapMax` | 64 | next change |

`GET /api/config` also reports `product`, `fwVersion` and `maxFlaps`. Its `version` field is
the firmware version (same as `fwVersion`); clients key on `product` and the capability tokens.

> ### Test WiFi credentials are compiled in
> `DEFAULT_WIFI_SSID` / `DEFAULT_WIFI_PASS` in `src/common.h` seed the config on a board whose
> NVS has never had a network saved, so a freshly flashed unit joins the bench network without
> the SoftAP setup page. They are a development convenience, **not** a secret store — anyone
> with the firmware image can read the password. Blank them before publishing the firmware or
> the repository. Once any network is saved from the UI, NVS wins and they are never consulted
> again.

---

## Language

The dashboard speaks 13 languages. **One firmware image ships all of them** — switching
language never means reflashing, and English costs nothing extra because it *is* the text in
the page.

| | |
|---|---|
| English | `en` · `en-GB` · `en-AU` |
| Western Europe | `fr` · `de` · `es` · `it` · `pt` · `pt-BR` · `nl` |
| Nordics | `da` · `sv` · `nb` · `fi` |

The gateway stores **no language setting at all** — no config field, no NVS write, no API. The
choice is resolved in the browser, highest priority first:

1. **`?lang=fr` in the URL.** This lets the companion app request a language for an embedded
   view *without* touching the user's own preference, so it is deliberately not saved.
2. **The Settings → Language override**, kept in `localStorage` (per-device, like a bookmark).
3. **`navigator.languages`** — "Auto", the default. It follows the browser exactly the way the
   light/dark theme already does, so a French browser gets a French dashboard with no setup.
4. **English.**

A region falls back to its base (`fr-CA` → `fr`), and an unsupported language simply stays
English. `en-US` resolves to the **base** English, never to `en-GB` — the base is US spelling,
and `en-GB`/`en-AU` are thin diffs over it (`color` → `colour`).

Each language is one gzipped JSON dictionary in flash, fetched from `GET /lang/<code>` only if
it is the one being used. A key with no translation falls back to English on its own, so a
partial dictionary is a supported state rather than a broken page.

### Why these languages

The dashboard is UTF-8 and could render anything. The **flap modules cannot**: their glyphs are
Windows-1252 (see [The reel](#the-reel)). Translating the UI into a language whose alphabet the
wall itself can never show would be a promise the hardware cannot keep — so the language list is
scoped to what Windows-1252 covers.

### Changing the text

`ui/index.html` is the source of truth, and `src/web_ui.h` is **generated** from it:

```sh
python3 tools/i18n_extract.py --wrap   # English text  -> ui/strings/en.json (+ wrap composed messages)
python3 tools/i18n_context.py          # where each string appears -> ui/strings/CONTEXT.md
python3 tools/i18n_check.py            # validate every dictionary
python3 tools/build_ui.py              # ui/ -> src/web_ui.h        (--check in CI)
node    tools/i18n_test.js             # language-resolution regression test
```

Never edit `src/web_ui.h` by hand — the next `build_ui.py` overwrites it.

Strings are keyed by their **English text**, so the markup needs no `data-i18n` tagging: a DOM
walk plus a `MutationObserver` translates the static page *and* whatever the JS builds later
(the flap wall, the quiet-day checkboxes). Messages the JS *composes*
(`"Error: " + e`) are wrapped in
`t()`, because a walk only ever sees the finished string.

---

## Compatibility

The companion app's *core* contract is seven HTTP endpoints, and it models *nothing*
about a module — not the flap count, not the character set, not serial numbers. The
reel is invisible to it. Those seven are
`GET /api/config`, `GET /api/status`, `POST /api/frames/send`, `POST /api/frames/batch`,
`POST /api/companion`, and `GET`/`PUT /api/companion/settings`. (Since v1.22.0 the
`/api/frames/*` pair is the only send surface — the physical gateway's paths it once
aliased are gone.) Beyond that core the companion *feature-detects* through
`GET /api/capabilities`: on this gateway it finds and uses the index-addressed cell
surface (`POST /api/display/cells`) and the whole canvas family — QOI frames, delta
rects, the stream channel, the atlas library, transitions.

Two things do matter, and both are handled:

1. **`GET /api/config` must report `version >= 3.1`.** The companion parses `MAJOR.MINOR` out of
   it and gates its gateway-stored settings on `>= (3,1)`. This firmware implements that surface
   exactly, so it answers `3.1.0` and puts its own version in `fwVersion`.
2. **`POST /api/frames/send` and `/api/frames/batch` must forward frame bytes verbatim.** The
   companion sends `m00-A\n` style frames as `windows-1252`, one byte per glyph. They are not
   transcoded.

The `/api/companion/settings` gzip blob store is carried over untouched. (The physical
gateway's MQTT surface and Home Assistant discovery are **not** — they were removed from
this firmware in v3.0.)

---

## Repository contents

```
src/common.h        board config, panel defaults, buffer sizes, shared types
src/gateway.h       umbrella header: common.h plus every subsystem's public API
src/globals.cpp     single definition site for every shared global
src/config.*        runtime configuration (GwConfig) persisted in NVS
src/rtc.*           wall-clock time: the system clock, set by NTP (no RTC chip)
src/touch.*         GT911 capacitive touch: taps + double-tap dismiss
src/charset.*       UTF-8 <-> Windows-1252 flap-byte transcoding
src/reel.h          the 237-flap reel and its two resolvers — Arduino-free, so
                    tools/reel_test.cpp compiles the same code
src/font1252.*      GENERATED bitmap glyphs: the 216 printable CP1252 flaps + 14 pictographs
src/aafont.h        GENERATED by tools/genaafont.py — Orbitron faces for the clock effect
src/frames.*        frame sanitization, the command log, the frameSend() choke point,
                    scheduled batch pacing
src/vmodule.*       the virtual split-flap modules: protocol dispatch and the shared reel
src/display.*       flap-wall geometry and the flap renderer (calls panel.*)
src/canvas.*        raw canvas: frames, rects, QOI decode, draw ops, on-device animation + ticker,
                    the animation/font libraries, transitions, sprite atlas, GIF import
src/audio.*         microphone frontend: ES8311 ADC capture, I2S, FFT/beat features
src/effects.*       on-device effects: plasma, fire, matrix, flip-o-rama, clock, Life,
                    spectrum + soundwall (audio-reactive, v3.4)
src/panel.*         the low-level panel driver: ESP32-P4 MIPI-DSI + PPA rotate
src/modules.*       high-level protocol send helpers (text/char/home) + FATFS mount
src/httpx.*         the native esp_http_server layer: route table, dispatch hook (CORS,
                    watchdog stamps), JSON/chunk/query/body helpers, heap-graded recv pacing
src/sse.*           GET /api/events: Server-Sent Events slots + the shared push buffer
src/web.*           HTTP server: dashboard (web_ui.h) + REST API + GET /lang/<code>
src/web_ui.h        GENERATED by tools/build_ui.py — do not edit
src/ota.*           firmware update: raw-body browser/curl upload + mDNS
src/tasks.*         the FreeRTOS task loops
src/main.cpp        setup() boot sequence + loop() watchdog supervisor

ui/index.html       the dashboard (HTML + CSS + JS) — the source of truth
ui/strings/en.json  the English string catalog, extracted from ui/index.html
ui/strings/*.json   one translation dictionary per language, keyed by the English text
ui/strings/CONTEXT.md  GENERATED — where each string appears, for translators

tools/build_ui.py   ui/ -> src/web_ui.h (page + gzipped dictionaries)
tools/i18n_extract.py  builds en.json; --wrap wraps composed JS messages in t()
tools/i18n_check.py    validates the dictionaries (stale keys, lost product name, encoding)
tools/i18n_context.py  builds CONTEXT.md
tools/i18n_test.js     regression test for language resolution and t()
tools/genfont.py    regenerates src/font1252.cpp from the vendored BDFs
tools/fontpack.py   packs a BDF into an MPFT blob for PUT /api/canvas/font
tools/genaafont.py  regenerates src/aafont.h from Orbitron (the clock effect's faces)
tools/Orbitron.ttf  vendored Orbitron variable font (SIL Open Font License)
tools/bdf/          public-domain X11 "misc-fixed" fonts (10x20, 9x18, 8x13, 6x13, 6x10, 6x9, 5x8)
tools/reel_test.cpp native regression test for the reel and its two resolvers

platformio.ini      build/upload configuration (single env: waveshare_matrix)
partitions-32MB.csv 4 MB app0 + 4 MB app1 + 23.9 MB FATFS — no tinyuf2 slot
ARCHITECTURE.md     why the non-obvious decisions were made
openapi.yaml        REST API reference
```

`modules.*` is the gateway's side of the module protocol — how a character, an index or a home
becomes a frame. `vmodule.*` is what acts on those frames. They talk only through protocol
frames, which is exactly why the port works. (Upstream, `modules.*` also holds a *registry* of
whatever is out on its serial wire. That has no meaning on a drawn wall and was removed in 1.10.)

---

## Building

One PlatformIO environment, `waveshare_matrix` (32 MB octal flash, `memory_type = opi_opi`,
`partitions-32MB.csv`):

```sh
pio run                 # build
pio run -t upload       # flash over USB (esptool)
pio device monitor      # 115200 baud, native USB CDC
```

**The first build recompiles the Arduino core** (v3.3.0): `platformio.ini` carries a
`custom_sdkconfig` block that rebuilds the core libraries with WiFi/lwIP buffers in PSRAM
(see RELEASE_NOTES.md and ARCHITECTURE.md for why). Expect ~10–20 minutes and network
access (IDF component fetches) the first time; later builds reuse the compiled core. If a
core rebuild fails half-way, wipe its artifacts and retry:

```sh
rm -rf .pio/build managed_components sdkconfig.defaults sdkconfig.waveshare_matrix
pio run
```

To return to the stock prebuilt core, delete the `custom_sdkconfig` block and do the same
wipe — no source changes are needed.

There is **no UF2 bootloader** on this board, so no drag-and-drop recovery: if a flash goes
wrong, hold **BOOT** while plugging in USB and run `pio run -t upload` (esptool) again. Once a
working firmware is on, web OTA (`/ota`) updates it as before.

After changing anything under `ui/`, regenerate the dashboard header — `pio run` compiles
`src/web_ui.h`, not `ui/index.html`, so skipping this silently builds the *old* page:

```sh
python3 tools/build_ui.py           # ui/ -> src/web_ui.h
python3 tools/build_ui.py --check   # or just assert it is up to date (CI)
```

Regenerate the fonts (only needed if you swap a BDF):

```sh
python3 tools/genfont.py
```

Run the reel regression test on the host:

```sh
c++ -std=c++17 -Isrc tools/reel_test.cpp src/charset.cpp src/font1252.cpp \
    -o /tmp/reel_test && /tmp/reel_test
```

---

## Known limitations

- **A full 1280×800 frame is ~2 MB, and rotating it into the scanout costs ~12 ms.** That is
  the inherent floor for driving a panel this size with a mandatory 90° rotate, so shared
  throughput tops out around 60–80 fps. Effects sidestep it by rendering at a 256×160 surface
  the PPA upscales; full-frame companion pushes and canvas transitions pay it directly. Prefer
  QOI or the `rects` partial-update channel over raw full frames, and use Ethernet for
  canvas-heavy work — the C6 WiFi link is the throughput bottleneck, not the panel.
- **WiFi is remoted through the ESP32-C6 over SDIO** (esp_hosted). Bulk inbound data (a canvas
  flood) queues in the C6's SDIO RX pool, which is internal/DMA RAM; `custom_sdkconfig` in
  `platformio.ini` keeps lwIP/pbuf allocations in PSRAM and bounds the TCP window so that pool
  never starves (it did, once, and the driver asserts rather than drops — the companion-crash
  fix). WiFi modem sleep stays disabled (`src/main.cpp`).
- **Wall-clock time needs a network.** This board has no RTC chip, so time is invalid from
  power-on until the first NTP sync. Every caller already handles that state; frame timestamps
  show `HH:MM:SS` uptime until then.

---

## Licence

CC BY-NC-SA 4.0, as the upstream Split-Flap Gateway. Split-flap module hardware and the initial
protocol by [Adam G Makes](https://www.youtube.com/@AdamGMakes). The bundled bitmap fonts are
the X11 `misc-fixed` faces: *"Public domain font. Share and enjoy."*
