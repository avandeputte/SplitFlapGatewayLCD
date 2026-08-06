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
#include <esp_async_memcpy.h>        // AXI-GDMA bulk copies (panelFastCopy, v0.3.3)
#include <hal/axi_icm_ll.h>          // AXI arbiter QoS: the scan-out must WIN arbitration (v0.4.1)
#include <soc/mipi_dsi_bridge_struct.h>  // the bridge's underflow substitute pixel (v0.4.1)
#include <esp_lcd_mipi_dsi.h>        // refresh-done heartbeat callbacks (already included; harmless)
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
// Pipelined present (v0.3.2): the full-res rotate is ~53 ms of pure engine time (16x16
// macro-blocks -> 32-36 B PSRAM transactions; a hardware floor on this silicon rev). Run it
// NON-blocking: panelShow waits for the PREVIOUS rotate, kicks this one, and returns -- the
// app composes the next frame while the engine churns. Safe because the just-kicked source
// becomes liveBuf, which everything only READS; the writers of that memory (panelSetScale,
// panelFreeAll) drain the pipeline first via panelRotSync().
static SemaphoreHandle_t         gRotDone = nullptr;    // given when the in-flight rotate lands
static bool IRAM_ATTR ppaRotDoneCb(ppa_client_handle_t, ppa_event_data_t*, void* ud) {
  BaseType_t hp = pdFALSE;
  if (ud) xSemaphoreGiveFromISR((SemaphoreHandle_t)ud, &hp);
  return hp == pdTRUE;
}
// DSI wedge self-heal (v0.4.1): heartbeat counter bumped by the refresh ISR; panelShow
// stamps activity; panelHealthTick recovers a stalled pipeline (see there).
static volatile uint32_t gRefreshBeats = 0;
static volatile uint32_t gLastShowMs   = 0;
static bool IRAM_ATTR panelRefreshHeartbeatCb(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t*, void*) {
  gRefreshBeats++;
  return false;
}
static void panelRotSync() {                             // drain: wait out an in-flight rotate
  if (gPpa && gRotDone) { xSemaphoreTake(gRotDone, portMAX_DELAY); xSemaphoreGive(gRotDone); }
}
// Serialize the whole present. panelShow() is called from taskDisplay (core 1: wall/effects),
// taskWeb (core 0: canvas stream + ticker) and the httpd worker (core 0: one-shot frames). The
// single PPA client is NOT safe for concurrent use -- two presents at once (the window during an
// app switch, canvas released while a stream is still presenting) corrupt its transaction queue
// ("exceed maximum pending transactions") and can HARD-HANG the board. This mutex makes presents
// mutually exclusive across tasks. Recursive because panelBlipService() -> panelShow() re-enters
// on the same task.
static SemaphoreHandle_t         gShowMutex = nullptr;

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
  // Hold the present lock: a concurrent panelShow/panelPresentRects reading W/H/gScale
  // mid-flip would build a torn PPA config -- and the CPU fallback for a rejected op
  // can then index far past the scanout buffer (scale-5 coords against native W).
  if (gShowMutex) xSemaphoreTakeRecursive(gShowMutex, portMAX_DELAY);
  panelRotSync();                                // an in-flight rotate reads these buffers
  gScale = s;
  W = (uint16_t)(PANEL_NATIVE_H / s);
  H = (uint16_t)(PANEL_NATIVE_W / s);
  info.width = W; info.height = H;
  panelClearClip();
  memset(fb[0], 0, gFbBytes);
  memset(fb[1], 0, gFbBytes);
  if (gShowMutex) xSemaphoreGiveRecursive(gShowMutex);
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
bool panelBlendActive() { return gBlendActive; }   // fast-path gate for row-composed fills (v0.3.2)

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

// v0.3.2 perf: spans as direct row/column stores. Every span-based primitive (polygon
// fill, filled circles, thick lines) funnels through these; the old per-pixel panelPixel
// loop cost ~97 ms per seaweed polygon in the aquarium. Layer/blend fall back per-pixel.
void panelHLine(int x, int y, int w, uint8_t r, uint8_t g, uint8_t b) {
  if (!info.ok || y < 0 || y >= H || y < clipY0 || y >= clipY1) return;
  if (gLayerBuf) { for (int i = 0; i < w; i++) panelPixel(x + i, y, r, g, b); return; }
  if (gBlendActive) {
    // Blended span fast lane (v0.3.2): the aquarium's ADD-blended light-shaft quads are
    // 228K span pixels/frame; the per-pixel panelPixel fallback (~400 ns each: call +
    // bounds + readPixelRGB + blendCh calls) cost ~97 ms per quad. Same math, inlined
    // over the row: clip once, unpack-blend-pack per pixel, no per-pixel re-checks.
    if (x < clipX0) { w -= clipX0 - x; x = clipX0; }
    if (x < 0)      { w += x; x = 0; }
    if (x + w > W) w = W - x;
    if (x + w > clipX1) w = clipX1 - x;
    if (w <= 0) return;
    uint8_t sr = r, sg = g, sb = b;
    if (bgrOrder) { uint8_t t = sr; sr = sb; sb = t; }   // match pack565/readPixelRGB order
    const uint8_t m = gBlendMode, a = gBlendAlpha;
    px_t* p = fb[drawBuf] + (size_t)y * W + x;
    // Per-mode specialized loops (v0.3.3): hoist the mode/alpha switch OUT of the pixel
    // loop -- 3 blendCh calls per pixel were ~48 ms per light-shaft quad. Macros keep the
    // unpack/pack identical to readPixelRGB/pack565 so results match panelPixel exactly.
    #define SPAN_UNPACK(v) const uint8_t r5=((v)>>11)&0x1F, g6=((v)>>5)&0x3F, b5=(v)&0x1F;       const int dr=(r5<<3)|(r5>>2), dg=(g6<<2)|(g6>>4), db=(b5<<3)|(b5>>2)
    #define SPAN_PACK(nr,ng,nb) (px_t)((((nr)&0xF8)<<8) | (((ng)&0xFC)<<3) | (((unsigned)(nb))>>3))
    #define SPAN_LOOP(exprR,exprG,exprB) for (int i=0;i<w;i++){ const px_t v=p[i]; SPAN_UNPACK(v);       int nr=(exprR), ng=(exprG), nb=(exprB);       if(nr>255)nr=255; if(nr<0)nr=0; if(ng>255)ng=255; if(ng<0)ng=0; if(nb>255)nb=255; if(nb<0)nb=0;       p[i]=SPAN_PACK(nr,ng,nb); }
    const int AR = sr, AG = sg, AB = sb, AL = a;
    switch (m) {
      case 1:  SPAN_LOOP(dr + AR*AL/255, dg + AG*AL/255, db + AB*AL/255); break;              // add
      case 2:  SPAN_LOOP(((dr*AR/255)*AL + dr*(255-AL))/255,                                   // multiply
                         ((dg*AG/255)*AL + dg*(255-AL))/255,
                         ((db*AB/255)*AL + db*(255-AL))/255); break;
      case 3:  SPAN_LOOP(((255-(255-AR)*(255-dr)/255)*AL + dr*(255-AL))/255,                   // screen
                         ((255-(255-AG)*(255-dg)/255)*AL + dg*(255-AL))/255,
                         ((255-(255-AB)*(255-db)/255)*AL + db*(255-AL))/255); break;
      case 4:  SPAN_LOOP((AR*AL/255) > dr ? (AR*AL/255) : dr,                                  // max/lighten
                         (AG*AL/255) > dg ? (AG*AL/255) : dg,
                         (AB*AL/255) > db ? (AB*AL/255) : db); break;
      default: SPAN_LOOP((AR*AL + dr*(255-AL))/255,                                            // over
                         (AG*AL + dg*(255-AL))/255,
                         (AB*AL + db*(255-AL))/255); break;
    }
    #undef SPAN_LOOP
    #undef SPAN_PACK
    #undef SPAN_UNPACK
    return;
  }
  if (x < clipX0) { w -= clipX0 - x; x = clipX0; }
  if (x < 0)      { w += x; x = 0; }
  if (x + w > W) w = W - x;
  if (x + w > clipX1) w = clipX1 - x;
  if (w <= 0) return;
  const px_t v = pack565(r, g, b);
  px_t* p = fb[drawBuf] + (size_t)y * W + x;
  for (int i = 0; i < w; i++) p[i] = v;
}
void panelVLine(int x, int y, int h, uint8_t r, uint8_t g, uint8_t b) {
  if (!info.ok || x < 0 || x >= W || x < clipX0 || x >= clipX1) return;
  if (gLayerBuf || gBlendActive) { for (int i = 0; i < h; i++) panelPixel(x, y + i, r, g, b); return; }
  if (y < clipY0) { h -= clipY0 - y; y = clipY0; }
  if (y < 0)      { h += y; y = 0; }
  if (y + h > H) h = H - y;
  if (y + h > clipY1) h = clipY1 - y;
  if (h <= 0) return;
  const px_t v = pack565(r, g, b);
  px_t* p = fb[drawBuf] + (size_t)y * W + x;
  for (int i = 0; i < h; i++, p += W) *p = v;
}

void panelFillRect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
  // Fast path: a fill is the hottest canvas op. With a linear 565 buffer it is one
  // packed word written in a tight span loop -- no per-pixel calls.
  if (!info.ok) return;
  if (gBlendActive || gLayerBuf) {                 // blending / open layer: composite per pixel
    // Clip BEFORE the loop, not per pixel: wire dims are i16, and a hostile/buggy
    // 32767x32767 blended fill is a billion panelPixel calls -- multi-second, no
    // yield, on whichever task ran the op (TASK_WDT territory). The layer buffer
    // is surface-sized (W x H), so the surface clip is correct for it too.
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = (x + w > W) ? W : x + w, y1 = (y + h > H) ? H : y + h;
    if (x0 < clipX0) x0 = clipX0;
    if (y0 < clipY0) y0 = clipY0;
    if (x1 > clipX1) x1 = clipX1;
    if (y1 > clipY1) y1 = clipY1;
    for (int yy = y0; yy < y1; yy++)
      for (int xx = x0; xx < x1; xx++) panelPixel(xx, yy, r, g, b);
    return;
  }
  int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
  int x1 = (x + w > W) ? W : x + w, y1 = (y + h > H) ? H : y + h;
  if (x0 < clipX0) x0 = clipX0;
  if (y0 < clipY0) y0 = clipY0;
  if (x1 > clipX1) x1 = clipX1;
  if (y1 > clipY1) y1 = clipY1;
  if (x0 >= x1 || y0 >= y1) return;
  const px_t v = pack565(r, g, b);
  for (int yy = y0; yy < y1; yy++) {
    px_t* p = fb[drawBuf] + (size_t)yy * W + x0;
    for (int i = x1 - x0; i > 0; i--) *p++ = v;
  }
}

// AXI-GDMA bulk copy (v0.3.3): PSRAM->PSRAM at ~300-500 MB/s vs ~70 MB/s CPU memcpy, on an
// engine physically separate from the PPA's DMA2D pool (no contention with the pipelined
// rotate; the driver handles all cache coherency internally). Eligibility per the driver:
// both pointers AND the byte count must be multiples of the 128 B PSRAM burst; anything
// else falls back to plain memcpy. The ops task blocks on a semaphore, freeing the CPU.
static async_memcpy_handle_t gMcp = nullptr;
static SemaphoreHandle_t     gMcpDone = nullptr;
static bool IRAM_ATTR mcpDoneCb(async_memcpy_handle_t, async_memcpy_event_t*, void* ud) {
  BaseType_t hp = pdFALSE;
  xSemaphoreGiveFromISR((SemaphoreHandle_t)ud, &hp);
  return hp == pdTRUE;
}
void panelFastCopy(void* dst, const void* src, size_t bytes) {
  if (!gMcp) {                                   // lazy one-time install (AXI backend: PSRAM bursts)
    static bool tried = false;
    if (!tried) { tried = true;
      async_memcpy_config_t cfg = {};
      cfg.backlog = 1;
      cfg.dma_burst_size = 128;
      if (esp_async_memcpy_install_gdma_axi(&cfg, &gMcp) == ESP_OK) gMcpDone = xSemaphoreCreateBinary();
      else gMcp = nullptr;
    }
  }
  const bool aligned = ((uintptr_t)dst % 128 == 0) && ((uintptr_t)src % 128 == 0) && (bytes % 128 == 0);
  if (gMcp && gMcpDone && aligned && bytes >= 32768) {
    // Chunked with breathing gaps (v0.4.1): an unbroken 2 MB burst at 128 B bursts hogs
    // the PSRAM controller hard enough to starve the DPI scan-out -- sub-line underruns
    // are silently discarded by the bridge (no IRQ) and show as brief white flicker.
    // 256 KB chunks with a tick's gap keep the copy fast (~+2 ms total) while the DPI
    // refills between bursts. Also caps the DMA driver's per-call internal descriptor
    // allocation (~30 KB for 2 MB -> ~4 KB per chunk).
    const size_t CHUNK = 256 * 1024;
    uint8_t* d = (uint8_t*)dst; const uint8_t* sp = (const uint8_t*)src;
    size_t off = 0; bool ok = true;
    while (off < bytes && ok) {
      const size_t n = (bytes - off > CHUNK) ? CHUNK : (bytes - off);
      ok = esp_async_memcpy(gMcp, d + off, (void*)(sp + off), n, mcpDoneCb, gMcpDone) == ESP_OK;
      if (ok) {
        xSemaphoreTake(gMcpDone, portMAX_DELAY);
        off += n;
        if (off < bytes) vTaskDelay(1);          // the breathing gap for the scan-out
      }
    }
    if (ok) return;
    if (off < bytes) memcpy(d + off, sp + off, bytes - off);   // finish what the DMA didn't
    return;
  }
  memcpy(dst, src, bytes);
}

// Full-frame native snapshot/restore (v0.3.3): the static-prefix replay's storage. Both
// operate on fb[drawBuf] at the CURRENT logical geometry; the caller keys validity on W/H.
bool panelSnapshotFull(uint16_t** buf, size_t* cap) {
  if (!info.ok) return false;
  const size_t need = (size_t)W * H * sizeof(px_t);
  if (*cap < need) { free(*buf);                 // 128-aligned so panelFastCopy qualifies
    *buf = (uint16_t*)heap_caps_aligned_alloc(128, need, MALLOC_CAP_SPIRAM);
    *cap = *buf ? need : 0; }
  if (!*buf) return false;
  panelFastCopy(*buf, fb[drawBuf], need);
  return true;
}
void panelRestoreFull(const uint16_t* buf) {
  if (!info.ok || !buf) return;
  panelFastCopy(fb[drawBuf], buf, (size_t)W * H * sizeof(px_t));
}
// Full-frame native blit honoring clip/layer: the gradient cache's hit path. Only a truly
// unclipped, layer-free full frame takes the DMA shortcut; anything else lands row by row.
void panelFastCopyToFb(const uint16_t* buf, size_t px) {
  if (!info.ok || !buf || px != (size_t)W * H) return;
  const bool clipped = (clipX0 > 0 || clipY0 > 0 || clipX1 < W || clipY1 < H);
  if (!clipped && !gLayerBuf) { panelFastCopy(fb[drawBuf], buf, px * sizeof(px_t)); return; }
  for (int y = 0; y < H; y++) panelBlitRowNative(0, y, W, buf + (size_t)y * W);
}

// v0.3.2 perf: expose the packer + a native-565 row blit for the gradient cache. The cache
// stores pre-packed rows; a hit lands as row memcpys (~3-8 ms full-screen vs ~370 ms of
// per-pixel composition). Layer fallback unpacks per pixel so group compositing stays right.
uint16_t panelPack565(uint8_t r, uint8_t g, uint8_t b) { return pack565(r, g, b); }
bool panelLayerActive() { return gLayerBuf != nullptr; }
void panelBlitRowNative(int x, int y, int n, const uint16_t* row) {
  if (!info.ok || y < 0 || y >= H || y < clipY0 || y >= clipY1) return;
  if (gLayerBuf) {
    for (int i = 0; i < n; i++) {
      const uint16_t v = row[i];
      uint8_t r5 = (v >> 11) & 0x1F, g6 = (v >> 5) & 0x3F, b5 = v & 0x1F;
      uint8_t r = (uint8_t)((r5 << 3) | (r5 >> 2)), g = (uint8_t)((g6 << 2) | (g6 >> 4)), b = (uint8_t)((b5 << 3) | (b5 >> 2));
      if (bgrOrder) { uint8_t t = r; r = b; b = t; }   // undo the baked-in order for panelPixel
      panelPixel(x + i, y, r, g, b);
    }
    return;
  }
  if (x < clipX0) { row += clipX0 - x; n -= clipX0 - x; x = clipX0; }
  if (x < 0)      { row -= x; n += x; x = 0; }
  if (x + n > W) n = W - x;
  if (x + n > clipX1) n = clipX1 - x;
  if (n <= 0) return;
  memcpy(fb[drawBuf] + (size_t)y * W + x, row, (size_t)n * sizeof(px_t));
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

void panelBlitCoverRow(int x, int y, int n, const uint8_t* cov, uint8_t r, uint8_t g, uint8_t b) {
  if (!info.ok || y < 0 || y >= H || y < clipY0 || y >= clipY1) return;
  // An open layer or a non-'over' blend mode: route each covered pixel through the general
  // path so group compositing and add/multiply/screen stay correct. Coverage folds into the
  // op's own alpha. Rare for text (it usually draws 'over'), so the per-pixel setBlend is fine.
  if (gLayerBuf || (gBlendActive && gBlendMode != 0)) {
    const uint8_t sMode = gBlendMode, sAlpha = gBlendAlpha; const bool sActive = gBlendActive;
    for (int i = 0; i < n; i++) {
      uint16_t a = cov[i];
      if (!a) continue;
      if (sActive) a = (uint16_t)(a * sAlpha / 255);
      if (!a) continue;
      panelSetBlend(sMode, (uint8_t)a);
      panelPixel(x + i, y, r, g, b);
    }
    gBlendMode = sMode; gBlendAlpha = sAlpha; gBlendActive = sActive;   // restore batch state
    return;
  }
  // Fast path: composite 'over' straight into the back buffer, honoring clip + batch alpha.
  const uint8_t ba = gBlendActive ? gBlendAlpha : 255;   // gBlendMode == 0 here
  int i0 = 0, i1 = n;
  if (x + i0 < clipX0) i0 = clipX0 - x;
  if (x + i0 < 0)      i0 = -x;
  if (x + i1 > W)      i1 = W - x;
  if (x + i1 > clipX1) i1 = clipX1 - x;
  px_t* row = fb[drawBuf] + (size_t)y * W;
  for (int i = i0; i < i1; i++) {
    uint16_t a = cov[i];
    if (!a) continue;
    if (ba != 255) a = (uint16_t)(a * ba / 255);
    if (!a) continue;
    const int px = x + i;
    if (a >= 255) { row[px] = pack565(r, g, b); continue; }
    uint8_t dr, dg, db; readPixelRGB(drawBuf, px, y, dr, dg, db);
    row[px] = pack565(blendCh(0, r, dr, (uint8_t)a),
                      blendCh(0, g, dg, (uint8_t)a),
                      blendCh(0, b, db, (uint8_t)a));
  }
}

void panelBoxBlur(int x, int y, int w, int h, int radius) {
  if (!info.ok || radius < 1) return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > W) w = W - x;
  if (y + h > H) h = H - y;
  if (w <= 0 || h <= 0) return;
  if (radius > w) radius = w;
  if (radius > h) radius = h;
  const size_t n = (size_t)w * h * 3;
  uint8_t* a = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  uint8_t* b = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  if (!a || !b) { free(a); free(b); return; }
  // Read the region into `a` (rgb888).
  for (int j = 0; j < h; j++)
    for (int i = 0; i < w; i++) {
      uint8_t r, g, bl; readPixelRGB(drawBuf, x + i, y + j, r, g, bl);
      uint8_t* p = a + ((size_t)j * w + i) * 3; p[0] = r; p[1] = g; p[2] = bl;
    }
  const int win = 2 * radius + 1;
  // Horizontal pass a -> b: sliding-window running sum, edges clamped.
  for (int j = 0; j < h; j++) {
    const uint8_t* row = a + (size_t)j * w * 3;
    uint8_t* out = b + (size_t)j * w * 3;
    int sr = 0, sg = 0, sb = 0;
    for (int k = -radius; k <= radius; k++) { int c = k < 0 ? 0 : (k >= w ? w - 1 : k); const uint8_t* p = row + c * 3; sr += p[0]; sg += p[1]; sb += p[2]; }
    for (int i = 0; i < w; i++) {
      out[i*3] = (uint8_t)(sr / win); out[i*3+1] = (uint8_t)(sg / win); out[i*3+2] = (uint8_t)(sb / win);
      int rem = i - radius; rem = rem < 0 ? 0 : rem;
      int add = i + radius + 1; add = add >= w ? w - 1 : add;
      const uint8_t* pr = row + rem * 3; const uint8_t* pa = row + add * 3;
      sr += pa[0] - pr[0]; sg += pa[1] - pr[1]; sb += pa[2] - pr[2];
    }
  }
  // Vertical pass b -> a.
  for (int i = 0; i < w; i++) {
    int sr = 0, sg = 0, sb = 0;
    for (int k = -radius; k <= radius; k++) { int c = k < 0 ? 0 : (k >= h ? h - 1 : k); const uint8_t* p = b + ((size_t)c * w + i) * 3; sr += p[0]; sg += p[1]; sb += p[2]; }
    for (int j = 0; j < h; j++) {
      uint8_t* p = a + ((size_t)j * w + i) * 3; p[0] = (uint8_t)(sr / win); p[1] = (uint8_t)(sg / win); p[2] = (uint8_t)(sb / win);
      int rem = j - radius; rem = rem < 0 ? 0 : rem;
      int add = j + radius + 1; add = add >= h ? h - 1 : add;
      const uint8_t* pr = b + ((size_t)rem * w + i) * 3; const uint8_t* pa = b + ((size_t)add * w + i) * 3;
      sr += pa[0] - pr[0]; sg += pa[1] - pr[1]; sb += pa[2] - pr[2];
    }
  }
  // Write `a` back to the region, honoring the clip rect.
  for (int j = 0; j < h; j++) {
    const int yy = y + j; if (yy < clipY0 || yy >= clipY1) continue;
    for (int i = 0; i < w; i++) {
      const int xx = x + i; if (xx < clipX0 || xx >= clipX1) continue;
      const uint8_t* p = a + ((size_t)j * w + i) * 3;
      fb[drawBuf][(size_t)yy * W + xx] = pack565(p[0], p[1], p[2]);
    }
  }
  free(a); free(b);
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
  // Fast path: the live buffer already holds native-LE RGB565 (what the panel scans
  // out), so an rgb565 screenshot on an RGB panel is just a big-endian byte swap of
  // it -- no per-pixel unpack/repack. The BGR case still goes per-pixel to undo the
  // wiring swap into logical colours.
  if (rgb565 && !bgrOrder) {
    const px_t* src = fb[buf];
    const size_t n = (size_t)W * H;
    for (size_t i = 0; i < n; i++) { px_t v = src[i]; out[i*2] = (uint8_t)(v >> 8); out[i*2+1] = (uint8_t)v; }
    return;
  }
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
    // Pipelined: the caller (panelShow) has already TAKEN gRotDone, so at most one rotate
    // is in flight; the engine's done-callback gives it back. The submitted config is
    // copied by the driver at submit, so the stack-local op is safe.
    op.mode              = gRotDone ? PPA_TRANS_MODE_NON_BLOCKING : PPA_TRANS_MODE_BLOCKING;
    op.user_data         = (void*)gRotDone;
    const esp_err_t pe = ppa_do_scale_rotate_mirror(gPpa, &op);
    if (pe == ESP_OK) { if (!gRotDone) return; return; }
    if (gRotDone) xSemaphoreGive(gRotDone);   // submit failed: nothing in flight, release the gate
    // fall through to the CPU path on error -- but never SILENTLY (v0.3.2 perf hunt: a
    // silent per-frame fallback to the ~50 ms CPU rotate would masquerade as "slow app").
    { static uint32_t lastPpaErrMs = 0;
      if (millis() - lastPpaErrMs > 2000) { lastPpaErrMs = millis();
        printf("[PANEL] PPA rotate FAILED (0x%x) -- CPU fallback in use\n", (unsigned)pe); } }
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

// Incremental present (v0.4.2): rotate ONLY the given logical-space rects into the live
// scanout instead of the whole frame. The scanout persists, so unchanged content needs no
// traffic -- the aquarium's per-present PPA load drops from 4 MB to well under 1 MB, which
// removes the PSRAM contention that starved the DSI FIFO (the flash/blink saga). Single-
// buffer contract: does NOT swap drawBuf/liveBuf; the caller keeps drawing the same buffer.
// (No overlay hook, no blip -- effect-only path.) Falls back to per-rect CPU rotate.
void panelPresentRects(const int16_t* rects, int n) {
  if (!info.ok || n <= 0) return;
  if (gShowMutex) xSemaphoreTakeRecursive(gShowMutex, portMAX_DELAY);
  if (gPpa && gRotDone) xSemaphoreTake(gRotDone, portMAX_DELAY);   // any in-flight full rotate
  // Sync ONLY the dirty row bands (v0.4.2 blink fix): a full 2 MB esp_cache_msync runs
  // with interrupts disabled for ~1 ms -- longer than the ~0.75 ms vblank window in which
  // the DSI's frame re-arm ISR must land. A missed re-arm scans a whole frame of
  // substitute pixels: THE blink. Small per-rect row-band syncs keep every
  // interrupts-off window far below the vblank budget.
  for (int i = 0; gPpa && i < n; i++) {
    int y = rects[i*4+1], h = rects[i*4+3];
    if (y < 0) { h += y; y = 0; }
    if (y + h > H) h = H - y;
    if (h <= 0) continue;
    esp_cache_msync((uint8_t*)fb[drawBuf] + (size_t)y * W * sizeof(px_t),
                    (size_t)h * W * sizeof(px_t), ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  }
  for (int i = 0; i < n; i++) {
    int x = rects[i*4], y = rects[i*4+1], w = rects[i*4+2], h = rects[i*4+3];
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) continue;
    bool done = false;
    if (gPpa) {
      ppa_srm_oper_config_t op = {};
      op.in.buffer         = fb[drawBuf];
      op.in.pic_w          = W;
      op.in.pic_h          = H;
      op.in.block_w        = w;
      op.in.block_h        = h;
      op.in.block_offset_x = x;
      op.in.block_offset_y = y;
      op.in.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;
      op.out.buffer        = gScanout;
      op.out.buffer_size   = (size_t)PANEL_NATIVE_W * PANEL_NATIVE_H * sizeof(px_t);
      op.out.pic_w         = PANEL_NATIVE_W;
      op.out.pic_h         = PANEL_NATIVE_H;
      if (!PANEL_ROT_180) {                        // angle 90 (PPA convention): (x,y) -> (y, NH-(x+w))
        op.out.block_offset_x = y * gScale;
        op.out.block_offset_y = PANEL_NATIVE_H - (x + w) * gScale;
      } else {                                     // angle 270: (x,y) -> (NW-(y+h), x)
        op.out.block_offset_x = PANEL_NATIVE_W - (y + h) * gScale;
        op.out.block_offset_y = x * gScale;
      }
      op.out.srm_cm        = PPA_SRM_COLOR_MODE_RGB565;
      op.rotation_angle    = PANEL_ROT_180 ? PPA_SRM_ROTATION_ANGLE_270 : PPA_SRM_ROTATION_ANGLE_90;
      op.scale_x           = (float)gScale;
      op.scale_y           = (float)gScale;
      op.mode              = PPA_TRANS_MODE_BLOCKING;   // small blocks: total ~5-10 ms
      done = (ppa_do_scale_rotate_mirror(gPpa, &op) == ESP_OK);
    }
    if (!done) {                                   // CPU fallback, this rect only
      const px_t* src = fb[drawBuf];
      px_t* dst = (px_t*)gScanout;
      const int S = gScale, LW2 = W * S, LH2 = H * S;
      for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) {
          const px_t v = src[(size_t)yy * W + xx];
          for (int dy = 0; dy < S; dy++)
            for (int dx = 0; dx < S; dx++) {
              const int lx = xx * S + dx, ly = yy * S + dy;
              int nx, ny;
              if (!PANEL_ROT_180) { nx = LH2 - 1 - ly; ny = lx; }
              else                { nx = ly;           ny = LW2 - 1 - lx; }
              dst[(size_t)ny * PANEL_NATIVE_W + nx] = v;
            }
        }
      esp_cache_msync(gScanout, (size_t)PANEL_NATIVE_W * PANEL_NATIVE_H * sizeof(px_t),
                      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }
  }
  if (gPpa && gRotDone) xSemaphoreGive(gRotDone);  // blocking ops: the engine is idle again
  gLastShowMs = millis();                          // the wedge detector counts this as presenting
  if (gShowMutex) xSemaphoreGiveRecursive(gShowMutex);
}

void panelShow() {
  if (!info.ok) return;
  if (gShowMutex) xSemaphoreTakeRecursive(gShowMutex, portMAX_DELAY);   // one present at a time
  if (sOverlay) sOverlay();   // draw the overlay into the outgoing frame
  if (blipUntil && (int32_t)(millis() - blipUntil) < 0) { blipStamp(); liveHasBlip = true; }
  else liveHasBlip = false;

  // PPA reads physical PSRAM; the CPU drew through the cache. Flush before the blit --
  // only the LOGICAL region (at effect scale that is 82 KB, not the buffer's 2 MB).
  static uint32_t tSync = 0, tBlit = 0, nShow = 0, tLast = 0;   // bring-up: frame timing
  const uint32_t t0 = micros();
  // P4 cache lines are 128 B (the S3 was 64): a sync not aligned to that is REJECTED
  // outright by esp_cache_msync, not partially done -- the boot log said so.
  const size_t liveBytes = ((size_t)W * H * sizeof(px_t) + 127u) & ~((size_t)127u);
  if (gPpa && gRotDone) xSemaphoreTake(gRotDone, portMAX_DELAY);   // previous rotate must land first
  if (gPpa) {                                      // CHUNKED writeback (v0.4.2): one 2 MB msync
    const uint8_t* base = (const uint8_t*)fb[drawBuf];   // disables interrupts ~1 ms -- past the
    const size_t CH = 64 * 1024;                         // ~0.75 ms vblank re-arm budget = blink
    for (size_t off = 0; off < liveBytes; off += CH)
      esp_cache_msync((void*)(base + off), (liveBytes - off > CH) ? CH : liveBytes - off,
                      ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  }
  const uint32_t t1 = micros();
  rotateToScanout();
  const uint32_t t2 = micros();
  // TEMP flash-hunt (v0.3 diag): log the ONSET of any bright full-frame present -- the
  // "app-switch flash". Samples EVERY present (no rate limit) so a flash lasting a single
  // frame (~16 ms) can't slip between samples; edge-triggered (not-bright -> bright) so a
  // sustained bright app logs once, not 60x/s. ~3000 sparse reads/present, negligible cost.
  {
    uint32_t sr = 0, sg = 0, sb = 0, cnt = 0;
    const px_t* p = fb[drawBuf];
    const size_t total = (size_t)W * H;
    for (size_t i = 0; i < total; i += 331) {
      const px_t v = p[i];
      sr += ((v >> 11) & 0x1F) << 3; sg += ((v >> 5) & 0x3F) << 2; sb += (v & 0x1F) << 3; cnt++;
    }
    static bool wasBright = false;
    if (cnt) {
      const uint32_t ar = sr / cnt, ag = sg / cnt, ab = sb / cnt, avg = (ar + ag + ab) / 3;
      const bool bright = (avg > 110);
      if (bright && !wasBright)
        printf("[FLASH] bright present avg=%lu R=%lu G=%lu B=%lu scale=%u t=%lu\n",
               (unsigned long)avg, (unsigned long)ar, (unsigned long)ag, (unsigned long)ab,
               (unsigned)gScale, (unsigned long)millis());
      wasBright = bright;
    }
  }
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
  gLastShowMs = millis();                        // wedge detector: we ARE presenting
  if (gShowMutex) xSemaphoreGiveRecursive(gShowMutex);
}

// ---- bring-up / teardown ----------------------------------------------------------
static void panelFreeAll() {
  panelRotSync();                                // never free memory the engine is reading
  for (int b = 0; b < 2; b++)
    if (fb[b]) { heap_caps_free(fb[b]); fb[b] = nullptr; }
  panelLayerDiscard();
}

bool panelBegin(uint16_t width, uint16_t height, uint8_t depth, bool fbPsram) {
  (void)depth; (void)fbPsram;                     // HUB75 concepts; the LCD is always RGB565/PSRAM
  if (!gShowMutex) gShowMutex = xSemaphoreCreateRecursiveMutex();   // serialize presents (see gShowMutex)
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
  dpiCfg.dpi_clock_freq_mhz = 80;                  // 60 Hz. DO NOT lower: a 66 MHz trial against
                                                   // the aquarium's bandwidth flicker LATCHED the
                                                   // bridge solid blue within a minute (v0.4.1).
                                                   // Bandwidth relief lives in the DRAW paths
                                                   // (dirty-rect restores), not the scan clock.
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
  ppaCfg.data_burst_length = PPA_DATA_BURST_LENGTH_64;   // v0.4.1: 128 B bursts head-of-line
                                                         // block the DSI fetch; 64 B halves the
                                                         // unpreemptible window (ext. precedent:
                                                         // lvgl#9590 -- no throughput loss)
  if (ppa_register_client(&ppaCfg, &gPpa) != ESP_OK) {
    gPpa = nullptr;
    printf("[PANEL] PPA unavailable -- CPU rotate fallback\n");
  }
  // One-shot copy benchmark (v0.3.3): is the 2 MB blit descriptor-bound or bandwidth-bound?
  // Printed once at boot; fb[0]/fb[1] are 128-aligned PSRAM so the DMA path qualifies.
  { const size_t bytes = (size_t)W * H * sizeof(px_t);
    uint32_t t0 = micros(); memcpy(fb[1], fb[0], bytes);        uint32_t tCpu = micros() - t0;
    t0 = micros();          panelFastCopy(fb[1], fb[0], bytes); uint32_t tDma1 = micros() - t0;
    t0 = micros();          panelFastCopy(fb[1], fb[0], bytes); uint32_t tDma2 = micros() - t0;
    printf("[PANEL] copy bench %ukB: cpu %luus dma %luus/%luus (%s)\n",
           (unsigned)(bytes / 1024), (unsigned long)tCpu, (unsigned long)tDma1,
           (unsigned long)tDma2, gMcp ? "axi-gdma" : "fallback");
  }
  // THE white-flash mechanism (v0.4.1, confirmed by the red-pixel experiment): the DSI
  // bridge emits a hardware SUBSTITUTE PIXEL whenever its FIFO underflows -- register
  // dpi_rsv_dpi_data, default 0x3FFF = bright cyan-white, never programmed by esp_lcd.
  // Brief starvation showed as white flashes; sustained starvation is the solid-blue
  // latch (same mechanism, two durations). Recoloring it RED made the flashes red --
  // proof. Ship config: BLACK, so residual tail-latency underruns (which no bandwidth
  // or QoS tuning fully eliminates) are imperceptible blinks instead of white strobes.
  // The driver's discard_vcnt = h_size stays (underrun IRQ masked: its ESP_DRAM_LOGE in
  // ISR context is pure jitter in production). Must be re-applied after any DPI re-init.
  { MIPI_DSI_BRIDGE.dpi_rsv_dpi_data.dpi_rsv_data = 0x0000;      // substitute = black
    MIPI_DSI_BRIDGE.dpi_config_update.dpi_config_update = 1; }

  // Scan-out arbitration priority (v0.4.1): the DSI's framebuffer fetches ride DW-GDMA
  // master port 1 (the PSRAM-facing port); every AXI master defaults to QoS 0, so under
  // heavy PPA + memcpy traffic the DISPLAY lost arbitration and underran (white flicker;
  // occasionally a latched solid-blue bridge wedge). Priority 10 makes the ~160 MB/s scan
  // fetch win; the bulk movers just queue a little longer.
  axi_icm_ll_set_dw_gdma_qos_arbiter_prio(1, 10, 10);

  // Refresh heartbeat (v0.4.1): on this silicon rev the on_refresh_done callback fires
  // from the DMA re-arm ISR -- the exact loop that dies in the blue-wedge state -- so a
  // stalled counter WHILE presenting is a perfect wedge signal (see panelHealthTick).
  { esp_lcd_dpi_panel_event_callbacks_t hb = {};
    hb.on_refresh_done = panelRefreshHeartbeatCb;
    esp_lcd_dpi_panel_register_event_callbacks(gPanel, &hb, nullptr); }

  if (gPpa) {                                    // pipelined present (see gRotDone)
    if (!gRotDone) gRotDone = xSemaphoreCreateBinary();
    ppa_event_callbacks_t pcb = {};
    pcb.on_trans_done = ppaRotDoneCb;
    if (gRotDone && ppa_client_register_event_callbacks(gPpa, &pcb) == ESP_OK) xSemaphoreGive(gRotDone);
    else { vSemaphoreDelete(gRotDone); gRotDone = nullptr; }   // no callback -> stay blocking
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
void panelHealthTick() {
  // DSI blue-wedge self-heal (v0.4.1). The bridge's underrun ISR does NO recovery (it
  // only logs), and a hard underrun can desync the bridge's internal frame counter: the
  // DMA channel hangs mid-block, the re-arm loop dies, the panel goes undriven and the
  // JD9365 latches solid blue -- previously until a manual reboot. Detection: the
  // refresh heartbeat (driven by the very re-arm ISR that dies) stops advancing while
  // panelShow is still presenting. Recovery: esp_lcd_panel_init(gPanel) -- verified
  // idempotent and allocation-free in the DPI driver; it re-runs the full start-stream
  // sequence (re-arm DMA + bridge + video mode + DCS init). ~200-300 ms blink, no reboot.
  static uint32_t lastBeats = 0, lastCheckMs = 0, lastRecoverMs = 0;
  if (!info.ok || !gPanel) return;
  const uint32_t now = millis();
  if (now - lastCheckMs < 500) return;
  const uint32_t beats = gRefreshBeats;
  const bool presenting = (now - gLastShowMs) < 2000;
  const bool stalled = (beats == lastBeats);
  lastBeats = beats; lastCheckMs = now;
  if (!presenting || !stalled) return;
  if (now - lastRecoverMs < 5000) return;        // cooldown: never thrash the re-init
  lastRecoverMs = now;
  printf("[PANEL] DSI wedge: no refresh for 500+ ms while presenting -- re-initing the pipeline\n");
  // Hold the present lock for the WHOLE re-init: this runs on taskDisplay while
  // taskWeb / the httpd worker may be presenting -- re-arming the DPI DMA under a
  // live PPA submit corrupts the transaction queue (documented hard-hang).
  if (gShowMutex) xSemaphoreTakeRecursive(gShowMutex, portMAX_DELAY);
  panelRotSync();                                // let an in-flight PPA rotate land first
  if (esp_lcd_panel_init(gPanel) == ESP_OK) {
    printf("[PANEL] DSI pipeline recovered\n");
    MIPI_DSI_BRIDGE.dpi_rsv_dpi_data.dpi_rsv_data = 0x0000;      // re-apply: re-init resets it
    MIPI_DSI_BRIDGE.dpi_config_update.dpi_config_update = 1;
  } else printf("[PANEL] DSI re-init FAILED -- next stop: the task watchdog\n");
  if (gShowMutex) xSemaphoreGiveRecursive(gShowMutex);
}
