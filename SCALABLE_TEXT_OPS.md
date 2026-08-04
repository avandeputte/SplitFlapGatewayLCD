# Scalable on-device text — firmware proposal

## Why

Today every on-device face is a **fixed-size bitmap**: the bundled `Font1252` faces
(5×8 … 10×20), the uploaded `custom` MPFT face, and the three fixed Orbitron AA sizes
(34/24/13, `A–Z 0–9 :.-+%/`, folded to uppercase). Nothing scales.

That single limitation is why ~30 companion apps push whole pixel frames instead of
draw-ops: they need **large, fitted, mixed-case, anti-aliased** text (a 120 px clock, a
wrapped quote, a `72°`, an accented `Fühler`) that the on-device fonts can't produce. On
the LCD a frame is ~1 MB raw / tens of KB QOI; the same screen as a handful of `text` ops
is a few hundred bytes **and** renders crisp at the native 1280×800 instead of the ×5
upscale of the 256×160 logical panel.

**Add one capability — rasterize a bundled/uploaded TrueType face at an arbitrary pixel
size with anti-aliasing and the full CP1252 charset — and the whole text-app catalog can
move to ops.** This is the highest-leverage firmware change for the LCD (and it helps the
Matrix panel too: games gain real type instead of ≤20 px bitmap).

## Scope

**Converts to ops once this lands:** every "text + a little geometry" app — quotes, facts,
headlines, the clocks and counters, weather, crypto/stocks/exchange/sports tables, moon,
ISS, world-clock, and the channel screens.

**Stays frame-push, by nature (out of scope):** SD Photos and any per-pixel simulation
(Falling Sand) — inherently pixel data; their delta path is already efficient.

---

## 1. Bundled faces + the metrics contract

Rasterization uses [`stb_truetype.h`](https://github.com/nothings/stb) (single header,
tiny, no allocator surprises; the P4 has ample headroom, and a PSRAM glyph cache makes
repeated frames nearly free).

Bundle two faces in flash to start:

| id | family              | file (ship the exact bytes the companion uses) |
|----|---------------------|------------------------------------------------|
| 0  | sans-bold (default) | `DejaVuSans-Bold.ttf`                          |
| 1  | mono-bold           | `DejaVuSansMono-Bold.ttf`                      |
| 2  | custom (uploaded)   | a TTF PUT to `/api/canvas/font` (see §5)        |

**The metrics contract (critical).** The companion does all layout — wrap, fit-to-box,
alignment — using PIL against *its* copy of the font. The wall must rasterize the
**byte-identical** TTF so advance widths, ascent and descent agree; otherwise a line the
companion fit to the box overflows or underfills on the panel. The companion already ships
`backend/app/fonts/DejaVuSans-Bold.ttf` — ship *that file* to firmware flash. **A font
change (different file, different version, subsetting) is a coordinated companion+firmware
release.** Advertise the bundled faces + a content hash so a mismatch is detectable:

```jsonc
// GET /api/capabilities  →  canvas.text2 (new)
"text2": {
  "scalable": true,
  "aa": true,
  "maxSize": 512,                       // px; u16 on the wire
  "charset": "cp1252",
  "faces": [
    {"id": 0, "name": "sans-bold", "hash": "sha256:1a2b…"},
    {"id": 1, "name": "mono-bold", "hash": "sha256:9f8e…"},
    {"id": 2, "name": "custom",    "hash": null}   // present only when one is uploaded
  ]
}
```

The companion gates on `canvas.text2.scalable` and refuses to emit `gtext` against a face
whose `hash` doesn't match its local file (falls back to a frame push).

---

## 2. The `gtext` op

Keep the existing bitmap `text` op (`0x10`) exactly as-is — it stays the path for the small
fixed faces and the Orbitron AA sizes. Add a **new** op so nothing about the current wire
format shifts.

### JSON (`POST /api/canvas/ops`, and the stream's `0x03` record)

```jsonc
{ "op": "gtext",
  "x": 40, "y": 96,          // anchor; see align
  "s": "Partly Cloudy 72°",  // UTF-8, rasterized from the face's CP1252 coverage
  "size": 48,                // px, 1..maxSize
  "face": "sans",            // "sans" | "mono" | "custom"  (default "sans")
  "color": [214, 226, 246],
  "align": "left",           // "left" | "center" | "right" about x
  "aa": true,                // default true; false = 1-bit threshold (no blend)
  "outline": [0, 0, 0],      // optional: 1px ring in this color under the fill
  "shadow": null,            // optional: +1,+1 drop in this color under the fill
  "tracking": 0              // optional: extra px between glyphs (default 0)
}
```

Backward-compatible reader: an unknown `face` falls back to `"sans"`; `size > maxSize`
clamps; `outline` and `shadow` are mutually independent and both optional.

### Binary (`opsBin`, stream `0x06`) — opcode `0x21`

Next free opcode after `0x19 SCALE`. Layout (big-endian, signed coords):

```
0x21
  x        : i16
  y        : i16
  size     : u16              ← widened from the bitmap op's u8 (native heroes exceed 255)
  face     : u8               ← 0 sans, 1 mono, 2 custom
  flags    : u8               ← bit0-1 align (0 L,1 C,2 R); bit2 AA; bit3 outline; bit4 shadow
  color    : u8 u8 u8
  [outline : u8 u8 u8]        ← present iff flags bit3
  [shadow  : u8 u8 u8]        ← present iff flags bit4
  slen     : u8               ← UTF-8 byte length, 1..255
  bytes    : u8[slen]
```

Transforms apply to the **anchor** via the existing `xfX/xfY` (so `translate`/`scale`
position glyph runs), but — as with the bitmap op — the transform does **not** scale the
glyph raster; the glyph size is `size`. (If you later want transform-scaled glyphs, read
`xfSX()` and multiply `size` by it before rasterizing — optional, not required for v1.)

---

## 3. Rasterization & compositing

1. **Coverage.** `stbtt_GetCodepointBitmap` (or SDF if you prefer, but plain AA coverage is
   enough) at `stbtt_ScaleForPixelHeight(size)`. Cache the 8-bit coverage bitmap (§4).
2. **Blend.** Per pixel, `out = bg + (fg - bg) * cov / 255`, straight alpha. Honor the batch
   alpha/blend mode already in `gOpsBlend`/`gBinAlpha` so `gtext` composites like every other
   op.
3. **Depth < 8.** Reuse the existing Bayer-4 dither (`ditherCh`, the gradient path) on the
   blended value so ramped edges don't band on the 3–4-bitplane panels.
4. **Outline** (flags bit3): stamp the glyph coverage at the four neighbours (±1,0)/(0,±1) in
   the outline color first, then the fill — same visual as the bitmap `text` op's 4-way ring,
   so apps that outline for contrast over busy art keep working.
5. **Shadow** (flags bit4): stamp the coverage once at (+1,+1) in the shadow color, then the
   fill.
6. **Align.** Measure the run width from the cached advances; `x` is the left edge (align 0),
   the center (align 1), or the right edge (align 2).

`aa:false` thresholds coverage at 0x80 → a 1-bit stamp (cheap, for tiny sizes or when a hard
edge is wanted).

---

## 4. Glyph cache contract

Repeated frames (a clock ticking, a scrolling list) must not re-rasterize. Cache in PSRAM:

- **Key:** `(faceId, size, codepoint)`.
- **Value:** `{w, h, xoff, yoff, advance, cov[w*h]}` (8-bit coverage).
- **Budget:** a fixed PSRAM ceiling (suggest 4 MB) with **LRU** eviction; a full-panel hero
  glyph at 400 px is ~160 KB, so the ceiling bounds worst case. Report usage in
  `/api/capabilities` debug if convenient.
- **Warm set:** pre-rasterize `0-9 : . ° space` at the first requested clock size — the
  common tick path then never allocates.
- **Invalidation:** clear the cache on a custom-font upload/delete and on a face reload.

Rasterizing is the only new per-glyph cost; with the cache the steady state is a coverage
blit, comparable to the existing sprite blit.

---

## 5. Custom uploaded face (extend `/api/canvas/font`)

The current endpoint installs an MPFT **bitmap** blob into the `custom` slot. Extend it (or
add a sibling) to accept a **TTF**: detect the `0x00010000`/`OTTO`/`true` magic, keep the
raw TTF bytes resident in PSRAM, and rasterize on demand exactly like the bundled faces
(`face:"custom"` / id 2). Keep MPFT working for back-compat. `/api/canvas/font/save` persists
the TTF to FATFS as today; `/api/canvas/fonts` lists it with `{name, kind:"ttf", hash}`.

---

## 6. Performance budget & acceptance

- **Throughput:** a typical converted screen is 3–8 runs, 10–60 glyphs. Target: a fully
  cache-warm screen composites in well under one 60 Hz frame; a cold hero line (≤ 12 glyphs
  at 400 px) rasterizes in < ~30 ms one time, then caches.
- **Correctness vs the companion:** for the same `(text, size, face)`, on-device advances
  and line width must match the companion's PIL metrics within ±1 px per line so alignment
  holds. (Same TTF ⇒ same metrics; this is really a "did we ship identical bytes" check.)

**Test vectors** (companion will assert against these):
1. `gtext "Partly Cloudy 72°" size=48 face=sans align=left color=#d6e2f6` — the `°` renders
   (CP1252 0xB0), mixed case intact.
2. A right-aligned column: `gtext "$117,234" / "$4,124" / "$212" size=28 face=mono align=right`
   at the same `x` — digits line up (tabular).
3. `gtext "23:34" size=200 face=sans` — hero clock, crisp AA, single cache entry per glyph.
4. `outline=[0,0,0]` over a gradient background — the ring reads; fill unshifted.
5. Repeat frame 3 with the minute changed — only the changed glyphs miss the cache.

---

## 7. Companion side (the other half of the contract)

For reference — this lands in `SplitFlapGatewayCompanion` in lockstep:

- **`device.from_capabilities`** parses `canvas.text2` → `caps.text_scalable`, `caps.text_max_size`,
  `caps.text_faces` (name→hash).
- **`canvas.CanvasSurface.text(...)`** gains `face=`, drops the 34 px AA ceiling, and routes to
  `gtext` when the wall advertises `text2.scalable` and the face hash matches the local font;
  otherwise it keeps the bitmap/Orbitron path (older firmware) or the app frame-pushes.
- **`canvas_codec.encode_ops_bin`** emits `0x1A` (u16 size, face byte) for scalable runs; the
  JSON `gtext` is the fallback when binary can't carry it (e.g. > 255-byte string, split into
  runs).
- **Per app:** the text apps swap `build a PIL image → canvas.frame(img)` for `canvas.text(...)`
  runs, and opt into `lcd_ops` so they draw live at native resolution. That refactor is the
  payoff and is separate, incremental work — one app at a time, each verified LED-byte-identical
  and against the LCD render harness.

---

## 8. Out of scope / later

- Transform-scaled glyph rasters (read `xfSX()` into `size`) — easy add once v1 is proven.
- Kerning pairs — `tracking` covers the common need; full kerning is optional.
- A third family (condensed/serif) — trivial once the pipeline exists; add on demand.
- Sub-pixel positioning — integer pens are fine at these sizes.

**Bottom line:** one renderer (`stb_truetype` + a PSRAM glyph cache), one new op (`gtext` /
`0x1A`), two bundled TTFs shipped byte-identical to the companion. That unlocks the entire
text-app catalog to draw on-device — cheaper on the wire and sharper on the glass.
