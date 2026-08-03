// panel.cpp -- the LCD output layer: ESP32-P4 MIPI-DSI to the Waveshare 10.1" panel.
//
// WHY THIS IS SIMPLE (compared to the Matrix Gateway's HUB75 driver this replaces)
// -------------------------------------------------------------------------------
// A HUB75 matrix has no memory: the driver must scan bitplanes into it forever, and
// that firmware earned every one of its 1100 lines (BCM chains, OE duty brightness,
// GDMA stall watchdogs). A DSI panel is the opposite: the JD9365 controller and the
// P4's DPI engine own the refresh entirely -- we keep a plain linear RGB565
// framebuffer, and presenting a frame is one rotate-blit into the DPI scanout
// buffer. Brightness is a real backlight (an I2C controller on the display FPC),
// so it still costs no colour levels.
//
// GEOMETRY
// --------
// The panel is natively 800x1280 PORTRAIT; the gateway mounts it landscape. All
// drawing happens in LOGICAL landscape space (W=1280, H=800); panelShow() rotates
// 90 degrees into the native scanout, in hardware via the P4's PPA
// (Pixel Processing Accelerator) with a CPU fallback. Flip PANEL_ROT_180 below if
// the physical mount turns out upside-down.
//
// BUFFER MODEL (same contract as the Matrix Gateway)
// --------------------------------------------------
// Two PSRAM buffers: drawBuf (the CPU draws) and liveBuf (a CPU-side copy of what
// is on screen -- panelCloneToBack, panelReadback and panelScroll read it). The DPI
// scanout buffer is a third, owned by esp_lcd; the CPU never reads it. panelShow
// copies draw -> scanout (rotating), then swaps the draw/live roles. The copy is
// synchronous, so unlike the HUB75 driver there is no tear-guard: after show, both
// CPU buffers are immediately writable.

#include "panel.h"
#include <math.h>          // sqrtf, for the ellipse scanlines

#include <Arduino.h>
#include <Wire.h>          // backlight controller (0x45 on the shared I2C bus)
#include <esp_heap_caps.h>
#include <esp_cache.h>
#include <esp_ldo_regulator.h>       // MIPI DPHY power rail (LDO channel 3)
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_io.h>
#include <driver/ppa.h>              // hardware rotate for the landscape mount
#include "esp_lcd_jd9365_10_1.h"     // vendored Waveshare panel driver (Apache-2.0)
#include "sdcard.h"        // sdLog: on-card event log

// ---- tunables ---------------------------------------------------------------------
#define MIPI_LDO_CHAN       3        // P4 D-PHY power rail (2.5 V on LDO channel 3)
#define MIPI_LDO_MV         2500
#define PANEL_NATIVE_W      800      // portrait native; logical W/H arrive rotated
#define PANEL_NATIVE_H      1280
#define PANEL_ROT_180       0        // set 1 if the landscape mount is upside-down

typedef uint16_t px_t;               // RGB565, little-endian native

static PanelInfo info = {false, 0, 0, 0, 0, 0};

static px_t*   fb[2]   = {nullptr, nullptr};   // logical landscape framebuffers (PSRAM)
static uint8_t drawBuf = 1;                    // buffer the CPU draws into
static uint8_t liveBuf = 0;                    // CPU-side copy of the displayed frame
static size_t  gFbBytes = 0;                   // per-buffer size, cache-line rounded

static uint16_t W = 0, H = 0;

static esp_lcd_panel_handle_t    gPanel   = nullptr;
static esp_lcd_panel_io_handle_t gDbiIo   = nullptr;
static esp_lcd_dsi_bus_handle_t  gDsiBus  = nullptr;
static esp_ldo_channel_handle_t  gPhyLdo  = nullptr;
static void*                     gScanout = nullptr;   // DPI frame buffer (esp_lcd owns it)
static ppa_client_handle_t       gPpa     = nullptr;   // hardware rotate; null = CPU fallback

// ---- backlight --------------------------------------------------------------------
// The display FPC carries a small controller at 0x45: reg 0x95 is the power-up
// sequence, reg 0x96 the backlight level (0..255). Runtime writes are deferred to
// panelBacklightService() -- the shared I2C bus has no lock, and taskRTC owns all
// runtime I2C on this firmware (same rule as the Matrix Gateway's sensors).
static volatile int gBlPending = -1;           // -1 idle, else 0..255 to write
static volatile uint8_t bright = 255;

static void blWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(LCD_BL_I2C_ADDR);
  Wire.write(reg); Wire.write(val);
  Wire.endTransmission();
}

void panelSetBrightness(uint8_t b) {
  if (b == bright) return;
  bright = b;
  gBlPending = b;                              // taskRTC picks it up within 100 ms
}

void panelBacklightService() {                 // taskRTC only (the I2C rule)
  const int p = gBlPending;
  if (p < 0) return;
  gBlPending = -1;
  blWrite(0x96, (uint8_t)p);
}

// ---- pixel core -------------------------------------------------------------------
// 565 by truncation on write, bit-replication on read: write(read(x)) is exact, which
// is what lets panelScroll shift a frame repeatedly without the colours drifting.
static bool bgrOrder = false;
void panelSetColourOrder(bool bgr) { bgrOrder = bgr; }

// Effect render scale (LCD Gateway): the pixel effects were authored for a ~256-wide
// LED matrix -- at native 1280x800 their spatial features shrink to shimmer and the
// pixel count is 16x their budget. panelSetScale(5) makes the drawing surface 256x160:
// every primitive, clone, scroll and readback operates in that logical space, and
// panelShow has the PPA scale 5x in the same hardware pass that rotates -- pattern
// size and frame rate fixed together, for free. Scale 1 is the native surface (wall,
// canvas, alerts). Switching memsets both buffers: old content is the wrong geometry.
static uint8_t gScale = 1;
void panelSetScale(uint8_t s) {
  if (s < 1) s = 1;
  if (s > 8) s = 8;
  if (!info.ok || s == gScale) return;
  gScale = s;
  W = (uint16_t)(PANEL_NATIVE_H / s);
  H = (uint16_t)(PANEL_NATIVE_W / s);
  info.width = W; info.height = H;
  panelClearClip();
  memset(fb[0], 0, gFbBytes);
  memset(fb[1], 0, gFbBytes);
}
uint8_t panelGetScale() { return gScale; }

static inline px_t pack565(uint8_t r, uint8_t g, uint8_t b) {
  if (bgrOrder) { uint8_t t = r; r = b; b = t; }
  return (px_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static inline void readPixelRGB(uint8_t buf, int x, int y, uint8_t& r, uint8_t& g, uint8_t& b) {
  const px_t v = fb[buf][(size_t)y * W + x];
  uint8_t r5 = (v >> 11) & 0x1F, g6 = (v >> 5) & 0x3F, b5 = v & 0x1F;
  r = (uint8_t)((r5 << 3) | (r5 >> 2));
  g = (uint8_t)((g6 << 2) | (g6 >> 4));
  b = (uint8_t)((b5 << 3) | (b5 >> 2));
  if (bgrOrder) { uint8_t t = r; r = b; b = t; }
}

// ---- clip / blend / layers (unchanged semantics from the Matrix Gateway) ----------
static int clipX0 = 0, clipY0 = 0, clipX1 = 1 << 14, clipY1 = 1 << 14;
void panelSetClip(int x0, int y0, int x1, int y1) {
  clipX0 = x0 < 0 ? 0 : x0;  clipY0 = y0 < 0 ? 0 : y0;
  clipX1 = x1;               clipY1 = y1;
}
void panelClearClip() { clipX0 = 0; clipY0 = 0; clipX1 = 1 << 14; clipY1 = 1 << 14; }

static bool    gBlendActive = false;
static uint8_t gBlendMode   = 0;
static uint8_t gBlendAlpha  = 255;

static inline uint8_t blendCh(uint8_t mode, int s, int d, int a) {
  int out;
  switch (mode) {
    case 1:  out = d + s * a / 255; break;                                   // add
    case 2:  { int m = d * s / 255; out = (m * a + d * (255 - a)) / 255; } break;   // multiply
    case 3:  { int sc = 255 - (255 - s) * (255 - d) / 255;
               out = (sc * a + d * (255 - a)) / 255; } break;                // screen
    case 4:  { int ss = s * a / 255; out = ss > d ? ss : d; } break;         // max / lighten
    default: out = (s * a + d * (255 - a)) / 255; break;                     // over
  }
  return (uint8_t)(out > 255 ? 255 : out < 0 ? 0 : out);
}

void panelSetBlend(uint8_t mode, uint8_t alpha) {
  gBlendMode = mode; gBlendAlpha = alpha;
  gBlendActive = (mode != 0 || alpha != 255);
}
void panelClearBlend() { gBlendActive = false; gBlendMode = 0; gBlendAlpha = 255; }

// Offscreen layers: while open, every drawing primitive redirects into a full-panel
// RGBA shadow; composite flattens the group back with one blend mode + group alpha.
static uint8_t* gLayerBuf = nullptr;
static bool panelLayerOpen() { return gLayerBuf != nullptr; }

bool panelLayerBegin() {
  if (gLayerBuf) { memset(gLayerBuf, 0, (size_t)W * H * 4); return true; }
  gLayerBuf = (uint8_t*)heap_caps_calloc((size_t)W * H * 4, 1, MALLOC_CAP_SPIRAM);
  return gLayerBuf != nullptr;
}
void panelLayerDiscard() {
  if (!gLayerBuf) return;
  free(gLayerBuf); gLayerBuf = nullptr;
}
void panelLayerComposite(int ox, int oy, uint8_t mode, uint8_t galpha) {
  if (!gLayerBuf) return;
  uint8_t* buf = gLayerBuf; gLayerBuf = nullptr;   // stop redirecting: draws hit the panel now
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      const uint8_t* px = &buf[((size_t)y * W + x) * 4];
      if (!px[3]) continue;                        // untouched: transparent
      const uint8_t a = (uint8_t)((int)px[3] * galpha / 255);
      if (!a) continue;
      panelSetBlend(mode, a);
      panelPixel(x + ox, y + oy, px[0], px[1], px[2]);
    }
  panelClearBlend();
  free(buf);
}

static inline bool layerWrite(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  if (!gLayerBuf) return false;
  if (x < 0 || y < 0 || x >= W || y >= H) return true;
  if (x < clipX0 || y < clipY0 || x >= clipX1 || y >= clipY1) return true;
  uint8_t* px = &gLayerBuf[((size_t)y * W + x) * 4];
  const uint8_t a = gBlendActive ? gBlendAlpha : 255;
  if (gBlendActive && px[3]) {
    px[0] = blendCh(gBlendMode, r, px[0], gBlendAlpha);
    px[1] = blendCh(gBlendMode, g, px[1], gBlendAlpha);
    px[2] = blendCh(gBlendMode, b, px[2], gBlendAlpha);
    if (a > px[3]) px[3] = a;
  } else { px[0] = r; px[1] = g; px[2] = b; px[3] = a; }
  return true;
}

// ---- drawing primitives -----------------------------------------------------------
void panelPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  if (!info.ok || x < 0 || y < 0 || x >= W || y >= H) return;
  if (x < clipX0 || y < clipY0 || x >= clipX1 || y >= clipY1) return;
  if (layerWrite(x, y, r, g, b)) return;
  if (gBlendActive) {
    uint8_t dr, dg, db; readPixelRGB(drawBuf, x, y, dr, dg, db);
    r = blendCh(gBlendMode, r, dr, gBlendAlpha);
    g = blendCh(gBlendMode, g, dg, gBlendAlpha);
    b = blendCh(gBlendMode, b, db, gBlendAlpha);
  }
  fb[drawBuf][(size_t)y * W + x] = pack565(r, g, b);
}

void panelClear() {
  if (!info.ok) return;
  if (panelLayerOpen()) { panelFillRect(0, 0, W, H, 0, 0, 0); return; }
  memset(fb[drawBuf], 0, (size_t)W * H * sizeof(px_t));
}

void panelHLine(int x, int y, int w, uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < w; i++) panelPixel(x + i, y, r, g, b);
}
void panelVLine(int x, int y, int h, uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < h; i++) panelPixel(x, y + i, r, g, b);
}

void panelFillRect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
  // Fast path: a fill is the hottest canvas op. With a linear 565 buffer it is one
  // packed word written in a tight span loop -- no per-pixel calls.
  if (!info.ok) return;
  if (gBlendActive || gLayerBuf) {                 // blending / open layer: composite per pixel
    for (int yy = 0; yy < h; yy++)
      for (int xx = 0; xx < w; xx++) panelPixel(x + xx, y + yy, r, g, b);
    return;
  }
  int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
  int x1 = (x + w > W) ? W : x + w, y1 = (y + h > H) ? H : y + h;
  if (x0 < clipX0) x0 = clipX0;  if (y0 < clipY0) y0 = clipY0;
  if (x1 > clipX1) x1 = clipX1;  if (y1 > clipY1) y1 = clipY1;
  if (x0 >= x1 || y0 >= y1) return;
  const px_t v = pack565(r, g, b);
  for (int yy = y0; yy < y1; yy++) {
    px_t* p = fb[drawBuf] + (size_t)yy * W + x0;
    for (int i = x1 - x0; i > 0; i--) *p++ = v;
  }
}

void panelBlitRow888(int x, int y, int n, const uint8_t* rgb) {
  if (!info.ok || y < 0 || y >= H || y < clipY0 || y >= clipY1) return;
  if (gLayerBuf) {
    for (int i = 0; i < n; i++) panelPixel(x + i, y, rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]);
    return;
  }
  if (x < clipX0) { rgb += 3 * (clipX0 - x); n -= clipX0 - x; x = clipX0; }
  if (x < 0)      { rgb -= 3 * x; n += x; x = 0; }
  if (x + n > W) n = W - x;
  if (x + n > clipX1) n = clipX1 - x;
  if (n <= 0) return;
  px_t* p = fb[drawBuf] + (size_t)y * W + x;
  for (int i = 0; i < n; i++) p[i] = pack565(rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]);
}

void panelBlitRow565(int x, int y, int n, const uint8_t* be565) {
  if (!info.ok || y < 0 || y >= H || y < clipY0 || y >= clipY1) return;
  if (gLayerBuf) {
    for (int i = 0; i < n; i++) {
      const uint16_t v = ((uint16_t)be565[i * 2] << 8) | be565[i * 2 + 1];
      panelPixel(x + i, y, (uint8_t)(((v >> 11) & 0x1F) << 3),
                 (uint8_t)(((v >> 5) & 0x3F) << 2), (uint8_t)((v & 0x1F) << 3));
    }
    return;
  }
  if (x < clipX0) { be565 += 2 * (clipX0 - x); n -= clipX0 - x; x = clipX0; }
  if (x < 0)      { be565 -= 2 * x; n += x; x = 0; }
  if (x + n > W) n = W - x;
  if (x + n > clipX1) n = clipX1 - x;
  if (n <= 0) return;
  px_t* p = fb[drawBuf] + (size_t)y * W + x;
  if (!bgrOrder) {
    // Wire 565 is big-endian and ours is native little-endian: one byte swap per pixel.
    for (int i = 0; i < n; i++) p[i] = (px_t)(((px_t)be565[i * 2] << 8) | be565[i * 2 + 1]);
  } else {
    for (int i = 0; i < n; i++) {
      const uint16_t v = ((uint16_t)be565[i * 2] << 8) | be565[i * 2 + 1];
      p[i] = pack565((uint8_t)(((v >> 11) & 0x1F) << 3),
                     (uint8_t)(((v >> 5) & 0x3F) << 2), (uint8_t)((v & 0x1F) << 3));
    }
  }
}

// Bresenham line from (x0,y0) to (x1,y1). panelPixel clamps, so off-panel endpoints are fine.
void panelLine(int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b) {
  if (!info.ok) return;
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    panelPixel(x0, y0, r, g, b);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

// Midpoint circle centred (cx,cy), radius rad -- an outline, or a filled disc.
void panelCircle(int cx, int cy, int rad, bool fill, uint8_t r, uint8_t g, uint8_t b) {
  if (!info.ok || rad < 0) return;
  int x = rad, y = 0, err = 1 - rad;
  while (x >= y) {
    if (fill) {
      panelHLine(cx - x, cy + y, 2 * x + 1, r, g, b);
      panelHLine(cx - x, cy - y, 2 * x + 1, r, g, b);
      panelHLine(cx - y, cy + x, 2 * y + 1, r, g, b);
      panelHLine(cx - y, cy - x, 2 * y + 1, r, g, b);
    } else {
      panelPixel(cx + x, cy + y, r, g, b); panelPixel(cx - x, cy + y, r, g, b);
      panelPixel(cx + x, cy - y, r, g, b); panelPixel(cx - x, cy - y, r, g, b);
      panelPixel(cx + y, cy + x, r, g, b); panelPixel(cx - y, cy + x, r, g, b);
      panelPixel(cx + y, cy - x, r, g, b); panelPixel(cx - y, cy - x, r, g, b);
    }
    y++;
    if (err < 0) err += 2 * y + 1;
    else { x--; err += 2 * (y - x) + 1; }
  }
}

static inline void iswap(int& a, int& b) { int t = a; a = b; b = t; }

// Quarter-arc helpers (Adafruit-GFX corner bitmask: 1=TL 2=TR 4=BR 8=BL) -- used by round rects.
static void drawCircleHelper(int cx, int cy, int rad, uint8_t corner, uint8_t r, uint8_t g, uint8_t b) {
  int f = 1 - rad, ddx = 1, ddy = -2 * rad, x = 0, y = rad;
  while (x < y) {
    if (f >= 0) { y--; ddy += 2; f += ddy; }
    x++; ddx += 2; f += ddx;
    if (corner & 0x4) { panelPixel(cx + x, cy + y, r, g, b); panelPixel(cx + y, cy + x, r, g, b); }
    if (corner & 0x2) { panelPixel(cx + x, cy - y, r, g, b); panelPixel(cx + y, cy - x, r, g, b); }
    if (corner & 0x8) { panelPixel(cx - y, cy + x, r, g, b); panelPixel(cx - x, cy + y, r, g, b); }
    if (corner & 0x1) { panelPixel(cx - y, cy - x, r, g, b); panelPixel(cx - x, cy - y, r, g, b); }
  }
}
static void fillCircleHelper(int cx, int cy, int rad, uint8_t corner, int delta, uint8_t r, uint8_t g, uint8_t b) {
  int f = 1 - rad, ddx = 1, ddy = -2 * rad, x = 0, y = rad, px = 0, py = rad;
  delta++;
  while (x < y) {
    if (f >= 0) { y--; ddy += 2; f += ddy; }
    x++; ddx += 2; f += ddx;
    if (x < y + 1) {
      if (corner & 1) panelVLine(cx + x, cy - y, 2 * y + delta, r, g, b);
      if (corner & 2) panelVLine(cx - x, cy - y, 2 * y + delta, r, g, b);
    }
    if (y != py) {
      if (corner & 1) panelVLine(cx + py, cy - px, 2 * px + delta, r, g, b);
      if (corner & 2) panelVLine(cx - py, cy - px, 2 * px + delta, r, g, b);
      py = y;
    }
    px = x;
  }
}

// Triangle (x0,y0)-(x1,y1)-(x2,y2); outline (three lines) or filled (Adafruit scanline algorithm).
void panelTriangle(int x0, int y0, int x1, int y1, int x2, int y2, bool fill, uint8_t r, uint8_t g, uint8_t b) {
  if (!info.ok) return;
  if (!fill) {
    panelLine(x0, y0, x1, y1, r, g, b); panelLine(x1, y1, x2, y2, r, g, b); panelLine(x2, y2, x0, y0, r, g, b);
    return;
  }
  if (y0 > y1) { iswap(y0, y1); iswap(x0, x1); }
  if (y1 > y2) { iswap(y2, y1); iswap(x2, x1); }
  if (y0 > y1) { iswap(y0, y1); iswap(x0, x1); }
  if (y0 == y2) {                                  // degenerate: a flat line
    int a = x0, bb = x0;
    if (x1 < a) a = x1; else if (x1 > bb) bb = x1;
    if (x2 < a) a = x2; else if (x2 > bb) bb = x2;
    panelHLine(a, y0, bb - a + 1, r, g, b); return;
  }
  int dx01 = x1 - x0, dy01 = y1 - y0, dx02 = x2 - x0, dy02 = y2 - y0, dx12 = x2 - x1, dy12 = y2 - y1;
  int sa = 0, sb = 0, y, last = (y1 == y2) ? y1 : y1 - 1;
  for (y = y0; y <= last; y++) {
    int a = x0 + sa / dy01, bb = x0 + sb / dy02;
    sa += dx01; sb += dx02;
    if (a > bb) iswap(a, bb);
    panelHLine(a, y, bb - a + 1, r, g, b);
  }
  sa = dx12 * (y - y1); sb = dx02 * (y - y0);
  for (; y <= y2; y++) {
    int a = x1 + sa / dy12, bb = x0 + sb / dy02;
    sa += dx12; sb += dx02;
    if (a > bb) iswap(a, bb);
    panelHLine(a, y, bb - a + 1, r, g, b);
  }
}

// Rounded rectangle, outline or filled (straight edges + four quarter-circle corners).
void panelRoundRect(int x, int y, int w, int h, int rad, bool fill, uint8_t r, uint8_t g, uint8_t b) {
  if (!info.ok || w <= 0 || h <= 0) return;
  int maxr = ((w < h) ? w : h) / 2;
  if (rad > maxr) rad = maxr;
  if (rad < 0) rad = 0;
  if (fill) {
    panelFillRect(x + rad, y, w - 2 * rad, h, r, g, b);
    fillCircleHelper(x + w - rad - 1, y + rad, rad, 1, h - 2 * rad - 1, r, g, b);
    fillCircleHelper(x + rad,         y + rad, rad, 2, h - 2 * rad - 1, r, g, b);
  } else {
    panelHLine(x + rad, y,         w - 2 * rad, r, g, b);
    panelHLine(x + rad, y + h - 1, w - 2 * rad, r, g, b);
    panelVLine(x,         y + rad, h - 2 * rad, r, g, b);
    panelVLine(x + w - 1, y + rad, h - 2 * rad, r, g, b);
    drawCircleHelper(x + rad,         y + rad,         rad, 1, r, g, b);
    drawCircleHelper(x + w - rad - 1, y + rad,         rad, 2, r, g, b);
    drawCircleHelper(x + w - rad - 1, y + h - rad - 1, rad, 4, r, g, b);
    drawCircleHelper(x + rad,         y + h - rad - 1, rad, 8, r, g, b);
  }
}

// Axis-aligned ellipse centred (cx,cy) with semi-axes (a,b). Scanline fill; outline sampled both
// by-row and by-column so the flat top/bottom and sides have no gaps.
void panelEllipse(int cx, int cy, int a, int b, bool fill, uint8_t r, uint8_t g, uint8_t bc) {
  if (!info.ok || a < 0 || b < 0) return;
  if (a == 0) { panelVLine(cx, cy - b, 2 * b + 1, r, g, bc); return; }
  if (b == 0) { panelHLine(cx - a, cy, 2 * a + 1, r, g, bc); return; }
  if (fill) {
    for (int dy = -b; dy <= b; dy++) {
      float t = 1.0f - (float)(dy * dy) / (float)(b * b);
      if (t < 0) continue;
      int dx = (int)(a * sqrtf(t) + 0.5f);
      panelHLine(cx - dx, cy + dy, 2 * dx + 1, r, g, bc);
    }
  } else {
    for (int dy = -b; dy <= b; dy++) {
      float t = 1.0f - (float)(dy * dy) / (float)(b * b);
      if (t < 0) continue;
      int dx = (int)(a * sqrtf(t) + 0.5f);
      panelPixel(cx + dx, cy + dy, r, g, bc); panelPixel(cx - dx, cy + dy, r, g, bc);
    }
    for (int dx = -a; dx <= a; dx++) {
      float t = 1.0f - (float)(dx * dx) / (float)(a * a);
      if (t < 0) continue;
      int dy = (int)(b * sqrtf(t) + 0.5f);
      panelPixel(cx + dx, cy + dy, r, g, bc); panelPixel(cx + dx, cy - dy, r, g, bc);
    }
  }
}

void panelCloneToBack() {
  if (!info.ok) return;
  memcpy(fb[drawBuf], fb[liveBuf], (size_t)W * H * sizeof(px_t));
}

void panelReadback(uint8_t* out, bool rgb565) {
  if (!info.ok || !out) return;
  const uint8_t buf = liveBuf;
  size_t o = 0;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      uint8_t r, g, b; readPixelRGB(buf, x, y, r, g, b);
      if (rgb565) {
        uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        out[o++] = (uint8_t)(v >> 8); out[o++] = (uint8_t)(v & 0xFF);
      } else { out[o++] = r; out[o++] = g; out[o++] = b; }
    }
}

// Shift the live frame into the back buffer by (dx,dy); vacated pixels get the fill colour.
void panelScroll(int dx, int dy, uint8_t fr, uint8_t fg, uint8_t fb_) {
  if (!info.ok) return;
  const uint8_t src = liveBuf;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      int sx = x - dx, sy = y - dy;
      if (sx >= 0 && sx < W && sy >= 0 && sy < H) {
        uint8_t r, g, b; readPixelRGB(src, sx, sy, r, g, b);
        panelPixel(x, y, r, g, b);
      } else panelPixel(x, y, fr, fg, fb_);
    }
}

// ---- overlay + gesture ack blip (same contract as the Matrix Gateway) -------------
static void (*sOverlay)(void) = nullptr;
void panelSetOverlay(void (*fn)(void)) { sOverlay = fn; }

// Blip: sized up from the Matrix Gateway's 4x4 -- on a 1280-wide panel a 4-pixel
// square is invisible from across the room.
static const int      BLIP_SZ = 24;
static const uint32_t BLIP_MS = 220;
static volatile uint32_t blipUntil = 0;
static uint8_t  blipRGB[3];
static bool     liveHasBlip = false;
static uint8_t  blipSave[BLIP_SZ * BLIP_SZ * 3];

void panelGestureBlip(uint8_t r, uint8_t g, uint8_t b) {
  blipRGB[0] = r; blipRGB[1] = g; blipRGB[2] = b;
  blipUntil = millis() + BLIP_MS;
}

static void blipStamp() {                 // panelShow only: drawBuf holds the final frame
  const int x0 = W - BLIP_SZ - 4, y0 = 4;
  size_t o = 0;
  for (int y = y0; y < y0 + BLIP_SZ; y++)
    for (int x = x0; x < x0 + BLIP_SZ; x++) {
      readPixelRGB(drawBuf, x, y, blipSave[o], blipSave[o + 1], blipSave[o + 2]);
      o += 3;
    }
  panelFillRect(x0, y0, BLIP_SZ, BLIP_SZ, blipRGB[0], blipRGB[1], blipRGB[2]);
}

void panelBlipService() {
  if (!info.ok || !blipUntil) return;
  const bool expired = (int32_t)(millis() - blipUntil) >= 0;
  if (!expired && !liveHasBlip) {
    panelCloneToBack();
    panelShow();
  } else if (expired) {
    if (liveHasBlip) {
      panelCloneToBack();
      const int x0 = W - BLIP_SZ - 4, y0 = 4;
      size_t o = 0;
      for (int y = y0; y < y0 + BLIP_SZ; y++)
        for (int x = x0; x < x0 + BLIP_SZ; x++) {
          panelPixel(x, y, blipSave[o], blipSave[o + 1], blipSave[o + 2]);
          o += 3;
        }
      blipUntil = 0;
      panelShow();
    } else blipUntil = 0;
  }
}

// ---- present ----------------------------------------------------------------------
// Rotate the logical landscape frame into the native portrait scanout. PPA does it in
// hardware; the CPU fallback is exact but ~an order of magnitude slower. Logical
// (x,y) -> native (nx,ny): a 90-degree turn, direction chosen so "landscape with the
// board's connectors down" reads upright; PANEL_ROT_180 flips it for the other mount.
static void rotateToScanout() {
  if (gPpa) {
    ppa_srm_oper_config_t op = {};
    op.in.buffer         = fb[drawBuf];
    op.in.pic_w          = W;
    op.in.pic_h          = H;
    op.in.block_w        = W;
    op.in.block_h        = H;
    op.in.block_offset_x = 0;
    op.in.block_offset_y = 0;
    op.in.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer        = gScanout;
    op.out.buffer_size   = (size_t)PANEL_NATIVE_W * PANEL_NATIVE_H * sizeof(px_t);
    op.out.pic_w         = PANEL_NATIVE_W;
    op.out.pic_h         = PANEL_NATIVE_H;
    op.out.block_offset_x = 0;
    op.out.block_offset_y = 0;
    op.out.srm_cm        = PPA_SRM_COLOR_MODE_RGB565;
    op.rotation_angle    = PANEL_ROT_180 ? PPA_SRM_ROTATION_ANGLE_270 : PPA_SRM_ROTATION_ANGLE_90;
    op.scale_x           = (float)gScale;
    op.scale_y           = (float)gScale;
    op.mode              = PPA_TRANS_MODE_BLOCKING;
    if (ppa_do_scale_rotate_mirror(gPpa, &op) == ESP_OK) return;
    // fall through to the CPU path on error
  }
  const px_t* src = fb[drawBuf];
  px_t* dst = (px_t*)gScanout;
  const int S = gScale, LW = W * S, LH = H * S;   // landscape-native span
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      const px_t v = src[(size_t)y * W + x];
      for (int dy = 0; dy < S; dy++)
        for (int dx = 0; dx < S; dx++) {
          const int lx = x * S + dx, ly = y * S + dy;   // native landscape coords
          int nx, ny;
          if (!PANEL_ROT_180) { nx = LH - 1 - ly;  ny = lx; }
          else                { nx = ly;           ny = LW - 1 - lx; }
          dst[(size_t)ny * PANEL_NATIVE_W + nx] = v;
        }
    }
  esp_cache_msync(gScanout, (size_t)PANEL_NATIVE_W * PANEL_NATIVE_H * sizeof(px_t),
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

void panelShow() {
  if (sOverlay) sOverlay();   // draw the overlay into the outgoing frame
  if (!info.ok) return;
  if (blipUntil && (int32_t)(millis() - blipUntil) < 0) { blipStamp(); liveHasBlip = true; }
  else liveHasBlip = false;

  // PPA reads physical PSRAM; the CPU drew through the cache. Flush before the blit --
  // only the LOGICAL region (at effect scale that is 82 KB, not the buffer's 2 MB).
  static uint32_t tSync = 0, tBlit = 0, nShow = 0, tLast = 0;   // bring-up: frame timing
  const uint32_t t0 = micros();
  // P4 cache lines are 128 B (the S3 was 64): a sync not aligned to that is REJECTED
  // outright by esp_cache_msync, not partially done -- the boot log said so.
  const size_t liveBytes = ((size_t)W * H * sizeof(px_t) + 127u) & ~((size_t)127u);
  if (gPpa) esp_cache_msync(fb[drawBuf], liveBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  const uint32_t t1 = micros();
  rotateToScanout();
  const uint32_t t2 = micros();
  tSync += t1 - t0; tBlit += t2 - t1; nShow++;
  if (!tLast) tLast = millis();
  if (gSerialDebug && millis() - tLast > 2000 && nShow > 5) {
    printf("[PANEL] show: %lu fps, msync %lu us, blit %lu us\n",
           (unsigned long)(nShow * 1000UL / (millis() - tLast)),
           (unsigned long)(tSync / nShow), (unsigned long)(tBlit / nShow));
    tSync = tBlit = 0; nShow = 0; tLast = millis();
  }

  // Swap roles: what we just presented becomes the live copy; the old live buffer is
  // the new (stale) draw buffer. Synchronous blit -- no tear-guard needed.
  const uint8_t shown = drawBuf;
  drawBuf = liveBuf;
  liveBuf = shown;
}

// ---- bring-up / teardown ----------------------------------------------------------
static void panelFreeAll() {
  for (int b = 0; b < 2; b++)
    if (fb[b]) { heap_caps_free(fb[b]); fb[b] = nullptr; }
  panelLayerDiscard();
}

bool panelBegin(uint16_t width, uint16_t height, uint8_t depth, bool fbPsram) {
  (void)depth; (void)fbPsram;                     // HUB75 concepts; the LCD is always RGB565/PSRAM
  info = {false, width, height, 16, 0, 0};
  // The logical geometry must be the native panel rotated. Anything else (a smaller
  // "window") is representable later; bring-up keeps the 1:1 mapping.
  if (width != PANEL_NATIVE_H || height != PANEL_NATIVE_W) {
    printf("[PANEL] %ux%u requested; this build drives the 10.1\" DSI panel only "
           "(landscape %ux%u) -- using native geometry\n",
           (unsigned)width, (unsigned)height, PANEL_NATIVE_H, PANEL_NATIVE_W);
  }
  W = PANEL_NATIVE_H;  H = PANEL_NATIVE_W;        // landscape logical space
  info.width = W; info.height = H;

  const size_t fbBytes = (size_t)W * H * sizeof(px_t);
  gFbBytes = (fbBytes + 127u) & ~((size_t)127u);
  for (int b = 0; b < 2; b++) {
    fb[b] = (px_t*)heap_caps_aligned_alloc(128, gFbBytes, MALLOC_CAP_SPIRAM);
    if (!fb[b]) {
      printf("[PANEL] %u B of PSRAM unavailable for framebuffer %d\n", (unsigned)gFbBytes, b);
      panelFreeAll(); return false;
    }
    memset(fb[b], 0, gFbBytes);
  }

  // Preflight: the display's own controller (backlight/power, I2C 0x45 on the FPC)
  // must ACK before anything touches DSI -- the JD9365 init READS the panel ID over
  // the bus, and against an unpowered panel that read blocks the boot forever
  // (observed on the bench with the display's power lead unplugged). No controller
  // = no display: run headless, loudly, and say what to check.
  Wire.beginTransmission(LCD_BL_I2C_ADDR);
  if (Wire.endTransmission() != 0) {
    printf("[PANEL] display controller (0x%02X) not answering on I2C -- display "
           "absent or not ready (check the FPC seating). Running headless\n",
           LCD_BL_I2C_ADDR);
    panelFreeAll(); return false;
  }
  // MIPI D-PHY power (the P4 routes it through an internal LDO channel).
  esp_ldo_channel_config_t ldo = {};
  ldo.chan_id = MIPI_LDO_CHAN;
  ldo.voltage_mv = MIPI_LDO_MV;
  if (esp_ldo_acquire_channel(&ldo, &gPhyLdo) != ESP_OK) {
    printf("[PANEL] MIPI D-PHY LDO acquire failed\n");
    panelFreeAll(); return false;
  }

  // Backlight controller power-up (display FPC, I2C 0x45): the sequence the vendor
  // driver performs, done here so the vendored file stays free of I2C plumbing.
  // setup() is single-threaded at this point -- the one context allowed to touch
  // the bus outside taskRTC.
  blWrite(0x95, 0x11); blWrite(0x95, 0x17);
  blWrite(0x96, 0x00);
  delay(100);
  blWrite(0x96, 0xFF);

  // Vendor macros (JD9365_PANEL_BUS_DSI_2CH_CONFIG etc.) are C designated-initializer
  // lists that C++ rejects; the same values, spelled out. Source of truth: the
  // vendored esp_lcd_jd9365_10_1.h.
  esp_lcd_dsi_bus_config_t busCfg = {};
  busCfg.bus_id = 0;
  busCfg.num_data_lanes = 2;
  busCfg.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
  busCfg.lane_bit_rate_mbps = 1500;
  if (esp_lcd_new_dsi_bus(&busCfg, &gDsiBus) != ESP_OK) {
    printf("[PANEL] DSI bus create failed\n");
    panelFreeAll(); return false;
  }
  esp_lcd_dbi_io_config_t dbiCfg = {};
  dbiCfg.virtual_channel = 0;
  dbiCfg.lcd_cmd_bits = 8;
  dbiCfg.lcd_param_bits = 8;
  if (esp_lcd_new_panel_io_dbi(gDsiBus, &dbiCfg, &gDbiIo) != ESP_OK) {
    printf("[PANEL] DBI io create failed\n");
    panelFreeAll(); return false;
  }

  esp_lcd_dpi_panel_config_t dpiCfg = {};
  dpiCfg.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
  dpiCfg.dpi_clock_freq_mhz = 80;                  // 60 Hz at the timing below
  dpiCfg.virtual_channel = 0;
  // IDF 5.5 takes in_color_format (the legacy pixel_format field is a separate,
  // unread member here); and Waveshare's own 5.5 configs do NOT enable use_dma2d.
  dpiCfg.in_color_format = LCD_COLOR_FMT_RGB565;
  dpiCfg.num_fbs = 1;
  dpiCfg.video_timing.h_size = PANEL_NATIVE_W;
  dpiCfg.video_timing.v_size = PANEL_NATIVE_H;
  dpiCfg.video_timing.hsync_back_porch = 20;
  dpiCfg.video_timing.hsync_pulse_width = 20;
  dpiCfg.video_timing.hsync_front_porch = 40;
  dpiCfg.video_timing.vsync_back_porch = 10;
  dpiCfg.video_timing.vsync_pulse_width = 4;
  dpiCfg.video_timing.vsync_front_porch = 30;
  jd9365_vendor_config_t vendor = {};
  vendor.mipi_config.dsi_bus = gDsiBus;
  vendor.mipi_config.dpi_config = &dpiCfg;
  vendor.mipi_config.lane_num = 2;
  esp_lcd_panel_dev_config_t devCfg = {};
  devCfg.reset_gpio_num = DSI_RESET_PIN;
  devCfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  devCfg.bits_per_pixel = 16;
  devCfg.vendor_config = &vendor;
  esp_err_t e = esp_lcd_new_panel_jd9365(gDbiIo, &devCfg, &gPanel);
  if (e == ESP_OK) e = esp_lcd_panel_reset(gPanel);
  if (e == ESP_OK) e = esp_lcd_panel_init(gPanel);
  if (e != ESP_OK) {
    printf("[PANEL] JD9365 init failed\n");
    panelFreeAll(); return false;
  }
  esp_lcd_panel_disp_on_off(gPanel, true);

  // The DPI engine scans its own frame buffer continuously; get its address so the
  // rotate-blit can target it directly.
  if (esp_lcd_dpi_panel_get_frame_buffer(gPanel, 1, &gScanout) != ESP_OK || !gScanout) {
    printf("[PANEL] DPI frame buffer unavailable\n");
    panelFreeAll(); return false;
  }

  // PPA client for the hardware rotate. Optional: on failure the CPU path serves.
  ppa_client_config_t ppaCfg = {};
  ppaCfg.oper_type = PPA_OPERATION_SRM;
  if (ppa_register_client(&ppaCfg, &gPpa) != ESP_OK) {
    gPpa = nullptr;
    printf("[PANEL] PPA unavailable -- CPU rotate fallback\n");
  }

  info.bytes = (uint32_t)(gFbBytes * 2);
  info.refreshHz = 60;                             // DPI timing, fixed
  info.ok = true;
  printf("[PANEL] DSI up: %ux%u landscape (native %ux%u), RGB565, %s rotate\n",
         (unsigned)W, (unsigned)H, PANEL_NATIVE_W, PANEL_NATIVE_H, gPpa ? "PPA" : "CPU");
  return true;
}

const PanelInfo& panelInfo() { return info; }
bool panelFbInPsram() { return info.ok; }          // always: the framebuffers live in PSRAM

void panelStop() {
  if (!info.ok) return;
  gBlPending = -1;
  blWrite(0x96, 0x00);                             // backlight off; scanout keeps running dark
}

void panelResume() {
  if (!info.ok) return;
  blWrite(0x96, bright);
}

void panelRelease() {
  if (info.ok) panelStop();
  panelFreeAll();
  info.ok = false;
}

// The DPI engine owns refresh end to end; there is no descriptor chain to freeze.
// Kept as a hook: if a DSI-underrun class of wedge ever shows up in the field, its
// detector goes here (esp_lcd exposes DPI event callbacks for it).
void panelHealthTick() {}
