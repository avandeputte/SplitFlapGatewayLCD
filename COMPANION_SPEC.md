# Companion integration spec — LCD Gateway (ESP32-P4 / 10.1" DSI)

This is the companion-side work that pairs with the LCD Gateway firmware (branch as of
2026-08-04). Everything the firmware side needs is **already implemented and verified on
hardware** — this document is the list of changes the companion must make to (1) stop the
"slow / panel offline" behaviour under heavy canvas apps, and (2) use the new scalable-text /
blur / sprite ops.

Nothing here changes an existing wire format. The new ops use new opcodes (`0x21`–`0x23`) and a
new capability block (`canvas.text2`); the readback change is a new optional query param. A
companion that ignores all of it keeps working exactly as today — it just keeps the perf bug.

The two Part 1 items are the priority: they are what makes the wall stop reading "offline" while
a heavy canvas app runs.

---

## Why (one paragraph)

The gateway's HTTP server has a **single worker**. Any request whose body or response is a full
1280×800 frame (~2 MB) holds that worker for ~2–4 s, and while it does, every other request —
including the transport's own liveness poll — queues behind it and times out, so the UI shows
the wall "offline." The firmware fixed everything it can on its side (the canvas *stream* now
renders on a second core and stays responsive; a downscaled-readback path exists). The remaining
two triggers are **companion choices**: polling the *full* readback for a live preview, and
pushing *one-shot 2 MB frame PUTs* for a frame app. Both have a cheap alternative the firmware
already supports.

---

## Part 1 — Performance fixes (stop the "offline" / slow)

### 1.1 Request a **downscaled** preview readback

**Problem.** For an on-device app with no companion-side frame (any `lcd_ops` app — aquarium,
the games, effects, tickers), the live preview calls `readback_png()` → `GET /api/canvas/frame`,
which returns the **full 2 MB** panel and is downscaled client-side. Polled ~1 Hz while a browser
watches, that 2 MB send blocks the HTTP worker and the wall reads offline.

**Firmware (done).** `GET /api/canvas/frame` now accepts **`?scale=N`** (`N` = 1…16) and returns
every `N`-th pixel, i.e. a `(W/N)×(H/N)` image. The snapshot is still full-res internally (a fast
local copy); only the bytes on the wire shrink. Response headers report the **downscaled**
dimensions:

```
X-Canvas-Width:  <W/N>
X-Canvas-Height: <H/N>
X-Canvas-Format: rgb565 | rgb888
```

Measured on hardware: `scale=1` → 2 048 000 B in ~2.2 s; `scale=4` → 128 000 B in ~0.13 s.

**Companion change.** In `canvas.py`:
- `get_frame()` / `readback_png()` — append `&scale=N` to the gateway URL. Pick `N` so the preview
  is ≈ 320 px wide: `N = max(1, round(panel_width / 320))` → `N=4` on the 1280-wide LCD.
- Use the `X-Canvas-Width/Height` response headers for the returned image's dimensions; **drop the
  client-side downscale** (the board already did it).
- Keep the ~1 s readback cache as-is.
- Feature-detect: only send `scale` when the wall advertises it. Advertise via a new
  `canvas.readback_scale: true` token if you want a clean gate; otherwise gate on FW ≥ this build.
  (A gateway that doesn't understand `scale` ignores the param and returns full-res, so sending it
  is safe either way — but the perf win only lands on a gateway that honours it.)

**Acceptance.** With an `lcd_ops` app running **and** a browser viewing its preview,
`GET /api/status` stays < 200 ms and the wall never flips to "offline."

---

### 1.2 Route **frame-push** apps through the draw stream

**Problem.** A frame-push app that sends **one-shot** `PUT /api/canvas/frame` (2 MB each) blocks
the single worker per frame. Weather does this in its warm-up / after a switch before the stream
is adopted, and it reads offline for as long as it stays on one-shot PUTs. (Confirmed in the
soak: a rapid switch into weather → `/api/status` timing out for 25 s+.)

**Firmware (already correct).** The persistent draw stream (`PUT /api/canvas/stream`, record
types: `0x01` full frame, `0x02` rects, `0x06` binary ops) is serviced by a **separate task on
the second core** — it does **not** touch the control worker. A frame delivered over the stream
renders with `/api/status` still at ~12 ms. This is the whole point of the stream.

**Companion change.** In `canvas.py` / the frame-push path (`_maybe_stream`, `canvas.frame()`):
- For a continuous frame-push app on a wall that advertises `canvas.stream`, **adopt the stream
  up front** — open it at (or before) the first draw, not "after we've seen it draw." The current
  lazy adoption leaves an initial window of one-shot 2 MB PUTs, which is exactly the offline
  window.
- **Never** send a one-shot **full-frame** (2 MB) PUT while an app is the active driver. Deltas
  (rects) are fine; a full keyframe must ride the stream (`0x01` record), not `PUT
  /api/canvas/frame`.
- On an app switch, make sure the stream is re-opened for the new app **before** it starts
  pushing full frames. (The firmware now evicts a previous app's lingering stream — a new
  `PUT /api/canvas/stream` supersedes an open one instead of `409`-ing — so the companion can
  open the new stream immediately; it no longer has to wait out the old one.)

**Acceptance.** Switching to weather (or any frame app) → within ~1 s
`GET /api/canvas/stream` shows `{"open": true, records climbing}`, and `/api/status` stays
< 200 ms throughout. No sustained one-shot `PUT /api/canvas/frame` traffic while an app is active.

---

## Part 2 — Scalable text + effects (new ops)

The firmware added a real glyph rasterizer (stb_truetype + a PSRAM glyph cache), so text is no
longer limited to the fixed bitmap cells / the Orbitron AA subset. This is what lets the
text-forward apps (quotes, clocks, weather, crypto/stocks/world-clock, headlines, …) draw as
**ops** instead of pushing whole PIL frames — which is both cheaper on the wire **and** sidesteps
the frame-PUT congestion in §1.2. Converting those apps is the payoff; it's incremental, one app
at a time.

### 2.1 Capability — `canvas.text2`

`GET /api/capabilities` → `canvas` now includes:

```jsonc
"ops": [ …, "text", "gtext", "blur", …, "sprite", … ],   // gtext + blur added
"text2": {
  "scalable": true,             // false if the bundled face failed to parse — then don't emit gtext
  "aa": true,
  "maxSize": 512,               // px; clamp size to this
  "charset": "cp1252",
  "outline": true,
  "shadow": true,
  "faces": ["sans", "mono", "custom"],
  "customLoaded": false         // true once a TTF has been uploaded to the custom slot
}
```

Parse into caps (e.g. `caps.text_scalable`, `caps.text_max_size`, `caps.text_faces`). Only emit
`gtext` when `text2.scalable` is true; otherwise fall back to the existing bitmap/Orbitron `text`
op or a frame push.

### 2.2 `gtext` — scalable, anti-aliased text

**JSON** (in `POST /api/canvas/ops` and the stream's `0x03` record):

```jsonc
{ "op": "gtext",
  "x": 40, "y": 96,             // anchor; (x,y) is the TOP-LEFT of the ascent box
  "s": "Partly Cloudy 72°",     // UTF-8; rasterized from the face's CP1252 coverage
  "size": 48,                   // px, 1..maxSize
  "face": "sans",               // "sans" | "mono" | "custom"  (default "sans")
  "color": [214, 226, 246],
  "align": "left",              // "left" | "center" | "right", about x
  "aa": true,                   // default true; false = hard 1-bit edge
  "outline": [0, 0, 0],         // optional: 1 px ring under the fill
  "shadow": null,               // optional: +1,+1 drop under the fill (ignored if outline set)
  "tracking": 0                 // optional: extra px between glyphs
}
```

**Binary** — opcode **`0x21`** (in `opsBin` / the stream's `0x06` record). Big-endian, signed coords:

```
0x21
  x        : i16
  y        : i16
  size     : u16                 ← widened from the bitmap op's u8
  face     : u8                  ← 0 sans, 1 mono, 2 custom
  flags    : u8                  ← bits0-1 align (0 L / 1 C / 2 R); bit2 AA; bit3 outline; bit4 shadow
  color    : u8 u8 u8
  [outline : u8 u8 u8]           ← present iff flags bit3
  [shadow  : u8 u8 u8]           ← present iff flags bit4
  slen     : u8                  ← UTF-8 byte length, 1..255
  bytes    : u8[slen]
```

Transforms (`translate`/`scale`/`rotate`) move the **anchor** via the existing affine matrix but
do **not** scale the glyph raster; the glyph size is `size`.

**The metrics contract — critical.** The firmware bundles a **CP1252 subset of DejaVuSans-Bold**
with **advance widths preserved**, so the on-device pen matches the companion's layout *only if the
companion lays out with the byte-identical DejaVuSans-Bold* — which it already ships
(`backend/app/fonts/DejaVuSans-Bold.ttf`). Do all wrap / fit-to-box / alignment with that exact
face and the wall's advances agree to ±1 px. **If the face ever changes (different file/version,
different subsetting), it is a coordinated companion + firmware release.** Advertise/verify a
content hash if you want a hard mismatch guard.

### 2.3 The mono face (coordination needed)

`face: "mono"` exists in the wire contract, but the firmware currently **falls back to sans**
for it — `DejaVuSansMono-Bold` is not bundled yet. To get real tabular figures (crypto, stocks,
exchange rates, world-clock, tides):

1. Companion: add `DejaVuSansMono-Bold.ttf` to its fonts and lay tabular text out with it.
2. Firmware: bundle the byte-identical `DejaVuSansMono-Bold` (there is a generator,
   `tools/genttf.py`, that emits the CP1252 subset; then define `TTF_HAVE_MONO`).

Until both land, `face: "mono"` renders correctly but in the sans face (wrong metrics for column
alignment). Treat it as a joint change.

### 2.4 `blur` — box-blur / scrim

**JSON:** `{ "op": "blur", "x": 0, "y": 300, "w": 1280, "h": 200, "r": 12 }` — separable box-blur
of the region, in place. **Binary `0x22`:** `x(i16) y(i16) w(u16) h(u16) radius(u8)`.

Use it to darken/soften busy art behind text (weather sky, dashboards) on-device — pair with an
alpha fill for a scrim, then `gtext` on top — instead of a client-side PIL Gaussian on a
frame push. This is the last piece that keeps weather/dashboard on frame-push; with it they can
go to ops.

### 2.5 Arbitrary sprite scale

The `sprite` op now accepts a **float** `scale` (was integer 1–4):
`{ "op": "sprite", "i": 3, "x": 100, "y": 100, "scale": 2.5, "flip": "h", "rot": 90 }`.
**Binary `0x23` SPRITE2:** `i(u16) x(i16) y(i16) flags(u8) scale(u16 8.8-fixed)` — flags: bit0
flipH, bit1 flipV, bits2-3 rot/90. The integer `0x11 SPRITE` path is unchanged (still the fast
path for whole 1–4).

### 2.6 Custom uploaded face (optional)

`face: "custom"` (id 2) is a runtime slot the firmware can fill from an uploaded TTF (detect the
`0x00010000` / `OTTO` / `true` magic, keep the bytes resident, rasterize on demand). When you wire
the upload path, `text2.customLoaded` flips to `true`. Not required for the bundled faces.

---

## Rollout order

1. **§1.1 readback `scale`** and **§1.2 stream-for-frame-apps** — these end the "offline"/slow
   pain. Small, isolated, high value. Do these first.
2. **§2.1–2.2 `gtext`** wiring + gate on `text2.scalable`. Then convert text apps to ops one at a
   time, each verified against the layout (advances match ±1 px) — this also *removes* those apps
   from the frame-push path, compounding the §1.2 win.
3. **§2.4 `blur`**, **§2.5 sprite scale** — as the apps that want them are converted.
4. **§2.3 mono face** — coordinated with a firmware bundling change.

## Firmware reference (already shipped, for cross-checking)

- Readback downscale, `?scale=N` + `X-Canvas-*` headers — `handleApiCanvasFrameGet`.
- Canvas render on core 1 (keeps control responsive under a heavy stream app).
- Stream eviction: a new `PUT /api/canvas/stream` supersedes a lingering one; a wall/flap command
  also evicts a stale stream.
- `gtext`/`0x21`, `blur`/`0x22`, `sprite` float + `SPRITE2`/`0x23`; `canvas.text2` capability.
- Faces: CP1252 subset of DejaVuSans-Bold bundled (metrics-preserving); mono falls back to sans
  pending its bundle.
