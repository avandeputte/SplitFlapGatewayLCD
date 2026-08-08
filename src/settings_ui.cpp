#include "gateway.h"
#include "panel.h"
#include "touch.h"
#include "ttf.h"
#include "sound.h"          // soundStop() when Sound is toggled off
#include "settings_ui.h"
#include <WiFi.h>
#if BOARD_HAS_ETH
#include <ETH.h>            // ETH.localIP() on the 10.1" (WiFi is down while ETH is up)
#endif

// settings_ui.cpp -- the pull-down settings shade (swipe down from the top edge to open). A
// full-screen touch UI rendered by taskDisplay as a display mode: touchPoint()'s live position
// drives slider drags, touchTapConsume() drives button/toggle taps. Opens with a slide-down
// animation; tapping a schedule row's time area opens a time-editor sub-view. Sliders apply
// live; everything is persisted to NVS on close.

// The brightness slider is shown ONLY on boards with a real backlight to drive. The 7B's
// EK79007 has an always-on backlight (no I2C controller, no GPIO PWM), so it's compiled out.
#define SETTINGS_HAS_BRIGHTNESS (LCD_BL_I2C_ADDR >= 0)
#define SETTINGS_ANIM_MS        190        // slide-down open animation duration

// ---- state ----
static bool     gOpen     = false;
static bool     gCfgDirty = false;  // a control changed -> persist to NVS on close
static bool     gPrevDown = false;  // touch edge tracking (finger down last frame)
static int      gDrag     = -1;     // slider being dragged: 0 bright, 1 vol, 2 flip; -1 none
static uint32_t gOpenMs   = 0;      // millis() at open, for the slide-down animation
static int      gEditSched = -1;    // schedule time-editor: -1 main panel, 0 quiet, 1 dim

bool settingsActive() { return gOpen; }

void settingsOpen() {
  if (gOpen) return;
  gOpen = true; gCfgDirty = false; gDrag = -1; gEditSched = -1;
  gOpenMs = millis();
  gPrevDown = true;                    // the opening swipe's finger is still down -> no down-edge
  touchTapConsume(nullptr, nullptr);   // drop any tap latched during the swipe/drag
}
void settingsClose() {
  if (!gOpen) return;
  gOpen = false; gEditSched = -1;
  if (gCfgDirty) { saveConfig(); gCfgDirty = false; }   // persist volume/flip/toggles/schedules
  // Force a repaint of whatever was underneath: an effect/canvas/anim resumes on its own once
  // the dispatch stops calling us, but the idle WALL only redraws when marked dirty.
  dispMarkDirty();
}

// ---- draw helpers (sans face, AA). align: 0 left, 1 center, 2 right. (x,y)=top-left of ascent box ----
static void sTxt(int x, int y, int size, const char* s, uint8_t r, uint8_t g, uint8_t b, int align) {
  ttfDrawText(x, y, size, TTF_SANS, s, align, r, g, b, true, 0, false, 0, 0, 0, false, 0, 0, 0);
}
static bool inRect(int px, int py, int x, int y, int w, int h) {
  return px >= x && px <= x + w && py >= y && py <= y + h;
}
static void sliderTrack(int x, int y, int w, int h, int* t0, int* t1, int* ty, int* th) {
  *t0 = x + (int)(w * 0.30f);
  *t1 = x + w - (int)(w * 0.15f);
  *th = h / 5; if (*th < 5) *th = 5;
  *ty = y + (h - *th) / 2;
}
static void drawSlider(int x, int y, int w, int h, const char* label, float frac, const char* val) {
  const int lsz = (int)(h * 0.44f);
  sTxt(x, y + (h - lsz) / 2, lsz, label, 208, 214, 226, 0);
  int t0, t1, ty, th; sliderTrack(x, y, w, h, &t0, &t1, &ty, &th);
  if (frac < 0) frac = 0; if (frac > 1) frac = 1;
  const int fw = (int)((t1 - t0) * frac);
  panelFillRect(t0, ty, t1 - t0, th, 52, 58, 74);
  panelFillRect(t0, ty, fw, th, 86, 150, 232);
  const int kw = (h / 5 < 8) ? 8 : h / 5;
  panelFillRect(t0 + fw - kw / 2, ty - th, kw, th * 3, 232, 236, 246);
  sTxt(x + w, y + (h - lsz) / 2, lsz, val, 232, 236, 246, 2);
}
// A schedule/toggle row: label (+ optional sub-line) on the left, an on/off pill on the right.
static void drawToggle(int x, int y, int w, int h, const char* label, bool on, const char* sub) {
  const int lsz = (int)(h * 0.42f);
  sTxt(x, y + (h - lsz) / 2 - (sub ? lsz / 2 : 0), lsz, label, 208, 214, 226, 0);
  if (sub) sTxt(x, y + (h - lsz) / 2 + lsz / 2 + 2, (int)(lsz * 0.72f), sub, 150, 160, 178, 0);
  const int pw = (int)(h * 1.7f), ph = (int)(h * 0.54f);
  const int px = x + w - pw, py = y + (h - ph) / 2;
  panelFillRect(px, py, pw, ph, on ? 64 : 66, on ? 168 : 72, on ? 92 : 88);
  const int k = ph - 6;
  panelFillRect(on ? px + pw - k - 3 : px + 3, py + 3, k, k, 246, 249, 252);
}
static void drawButton(int x, int y, int w, int h, const char* label, uint8_t r, uint8_t g, uint8_t b) {
  panelFillRect(x, y, w, h, r, g, b);
  sTxt(x + w / 2, y + (int)(h * 0.28f), (int)(h * 0.44f), label, 246, 249, 252, 1);
}

// ---- HH:MM helpers (cfg time strings are "HH:MM\0", char[6]) ----
static void timeParse(const char* s, int* h, int* m) { *h = (s[0]-'0')*10 + (s[1]-'0'); *m = (s[3]-'0')*10 + (s[4]-'0'); }
static void timeFmt(char* s, int h, int m) {
  h = ((h % 24) + 24) % 24; m = ((m % 60) + 60) % 60;
  s[0] = '0' + h / 10; s[1] = '0' + h % 10; s[2] = ':'; s[3] = '0' + m / 10; s[4] = '0' + m % 10; s[5] = 0;
}
// A [-] / [+] pair (value drawn by the caller between them). Returns -1 minus, +1 plus, 0 none.
static int stepPair(int minusX, int plusX, int y, int bw, int bh, bool tap, int tpx, int tpy) {
  drawButton(minusX, y, bw, bh, "-", 74, 82, 104);
  drawButton(plusX,  y, bw, bh, "+", 74, 82, 104);
  if (tap) {
    if (inRect(tpx, tpy, minusX, y, bw, bh)) return -1;
    if (inRect(tpx, tpy, plusX,  y, bw, bh)) return +1;
  }
  return 0;
}

// ---- schedule time editor (a sub-view of the shade) ----
static bool editorRender(bool tap, int tpx, int tpy) {
  const int W = gPanel.panelW, H = gPanel.panelH;
  const bool quiet = (gEditSched == 0);
  char* startS = quiet ? cfg.quietStart : cfg.dimStart;
  char* endS   = quiet ? cfg.quietEnd   : cfg.dimEnd;

  const int pad     = W / 60 + 6;
  const int hdrH    = (int)(H * 0.12f);
  const int titleSz = (int)(hdrH * 0.46f);
  const int backW   = W / 6, backH = (int)(hdrH * 0.60f);
  const int backX   = W - backW - pad, backY = (hdrH - backH) / 2;

  const int bw   = (int)(W * 0.075f), bh = (int)(H * 0.13f);   // stepper button
  const int valW = (int)(bw * 1.6f);
  const int rowGap = (int)(H * 0.06f);
  const int startY = hdrH + (H - hdrH - (bh * 2 + rowGap)) / 2;   // two rows, vertically centered
  const int endY   = startY + bh + rowGap;
  // hour group then minute group; a colon between
  const int hMinusX = (int)(W * 0.34f);
  const int hPlusX  = hMinusX + bw + valW;
  const int mMinusX = hPlusX + bw + (int)(W * 0.07f);
  const int mPlusX  = mMinusX + bw + valW;

  int sh, sm, eh, em; timeParse(startS, &sh, &sm); timeParse(endS, &eh, &em);

  // ---- hit-test ----
  if (tap) {
    if (inRect(tpx, tpy, backX, backY, backW, backH)) { gEditSched = -1; return true; }  // back to main
    int d;
    if ((d = stepPair(hMinusX, hPlusX, startY, bw, bh, true, tpx, tpy))) { sh += d; timeFmt(startS, sh, sm); gCfgDirty = true; }
    else if ((d = stepPair(mMinusX, mPlusX, startY, bw, bh, true, tpx, tpy))) { sm += d * 15; timeFmt(startS, sh, sm); gCfgDirty = true; }
    else if ((d = stepPair(hMinusX, hPlusX, endY, bw, bh, true, tpx, tpy)))   { eh += d; timeFmt(endS, eh, em); gCfgDirty = true; }
    else if ((d = stepPair(mMinusX, mPlusX, endY, bw, bh, true, tpx, tpy)))   { em += d * 15; timeFmt(endS, eh, em); gCfgDirty = true; }
    timeParse(startS, &sh, &sm); timeParse(endS, &eh, &em);   // re-read after any change
  }

  // ---- draw ----
  panelFillRect(0, 0, W, H, 16, 18, 24);
  panelFillRect(0, 0, W, hdrH, 30, 34, 46);
  panelFillRect(0, hdrH, W, 2, 70, 80, 110);
  sTxt(pad, (hdrH - titleSz) / 2, titleSz, quiet ? "Quiet schedule" : "Dim schedule", 235, 238, 245, 0);
  drawButton(backX, backY, backW, backH, "Back", 60, 68, 88);

  const int lblSz = (int)(bh * 0.40f), valSz = (int)(bh * 0.5f);
  char t[4];
  // Start row
  sTxt(pad, startY + (bh - lblSz) / 2, lblSz, "Start", 208, 214, 226, 0);
  stepPair(hMinusX, hPlusX, startY, bw, bh, false, 0, 0);
  snprintf(t, sizeof(t), "%02d", sh); sTxt(hPlusX - valW / 2, startY + (bh - valSz) / 2, valSz, t, 235, 238, 245, 1);
  sTxt(hPlusX + bw + (int)(W * 0.035f), startY + (bh - valSz) / 2, valSz, ":", 200, 206, 220, 1);
  stepPair(mMinusX, mPlusX, startY, bw, bh, false, 0, 0);
  snprintf(t, sizeof(t), "%02d", sm); sTxt(mPlusX - valW / 2, startY + (bh - valSz) / 2, valSz, t, 235, 238, 245, 1);
  // End row
  sTxt(pad, endY + (bh - lblSz) / 2, lblSz, "End", 208, 214, 226, 0);
  stepPair(hMinusX, hPlusX, endY, bw, bh, false, 0, 0);
  snprintf(t, sizeof(t), "%02d", eh); sTxt(hPlusX - valW / 2, endY + (bh - valSz) / 2, valSz, t, 235, 238, 245, 1);
  sTxt(hPlusX + bw + (int)(W * 0.035f), endY + (bh - valSz) / 2, valSz, ":", 200, 206, 220, 1);
  stepPair(mMinusX, mPlusX, endY, bw, bh, false, 0, 0);
  snprintf(t, sizeof(t), "%02d", em); sTxt(mPlusX - valW / 2, endY + (bh - valSz) / 2, valSz, t, 235, 238, 245, 1);

  panelShow();
  vTaskDelay(pdMS_TO_TICKS(30));
  return true;
}

bool settingsRender() {
  if (!gOpen) return false;
  const int W = gPanel.panelW, H = gPanel.panelH;
  if (gCanvasMode) dispReturnToWall();       // keep the shade above a companion takeover

  // ---- touch inputs ----
  uint16_t tpx = 0, tpy = 0; const bool tap  = touchTapConsume(&tpx, &tpy);
  uint16_t lx  = 0, ly  = 0; const bool down = touchPoint(&lx, &ly);
  const bool downEdge = down && !gPrevDown;
  gPrevDown = down;

  if (gEditSched >= 0) return editorRender(tap, tpx, tpy);   // sub-view owns the frame

  // ---- slide-down animation (main panel): the whole shade slides in from above ----
  const uint32_t el = millis() - gOpenMs;
  float p = (el >= SETTINGS_ANIM_MS) ? 1.0f : (float)el / SETTINGS_ANIM_MS;
  const float pe = 1.0f - (1.0f - p) * (1.0f - p);           // ease-out
  const int yOff = (int)(-(1.0f - pe) * H);
  const bool animating = (p < 1.0f);

  // ---- layout (positions include yOff; interaction is skipped while animating, so the offset
  //      only matters visually -- when settled, yOff == 0) ----
  const int pad     = W / 60 + 6;
  const int hdrH    = (int)(H * 0.12f);
  const int titleSz = (int)(hdrH * 0.46f);
  const int doneW   = W / 7, doneH = (int)(hdrH * 0.60f);
  const int doneX   = W - doneW - pad, doneY = (hdrH - doneH) / 2 + yOff;
  const int footH   = (int)(H * 0.11f);
  const int rbW     = W / 5, rbH = (int)(footH * 0.66f);
  const int rbX     = pad,   rbY = H - footH + (footH - rbH) / 2 + yOff;

  const int colX = pad, colW = W - 2 * pad;
  const int sH   = (int)(H * 0.085f);
  const int gap  = (int)(H * 0.013f);
  const int schH = (int)(sH * 0.92f);
  int yc = hdrH + pad + yOff;
#if SETTINGS_HAS_BRIGHTNESS
  const int yBright = yc; yc += sH + gap;
#endif
  const int yVol    = yc; yc += sH + gap;
  const int yFlip   = yc; yc += sH + gap;
  const int yTgl    = yc; yc += sH + gap;
  const int ySchQ   = yc; yc += schH + gap;
#if SETTINGS_HAS_BRIGHTNESS
  const int ySchD   = yc; yc += schH + gap;   // dim = a backlight feature -> backlight boards only
#endif
  const int yInfo   = yc;
  const int halfW   = (colW - pad) / 2;
  const int schPillW = (int)(schH * 1.7f);                  // schedule row: pill area = enable toggle

  // ---- slider drag (disabled while animating in) ----
  if (!down) gDrag = -1;
  if (downEdge && !animating) {
    gDrag = -1;
#if SETTINGS_HAS_BRIGHTNESS
    if (inRect(lx, ly, colX, yBright, colW, sH)) gDrag = 0;
#endif
    if (gDrag < 0) {
      if      (inRect(lx, ly, colX, yVol,  colW, sH)) gDrag = 1;
      else if (inRect(lx, ly, colX, yFlip, colW, sH)) gDrag = 2;
    }
  }
  if (down && gDrag >= 0 && !animating) {
    int rowY = (gDrag == 1) ? yVol : yFlip;
#if SETTINGS_HAS_BRIGHTNESS
    if (gDrag == 0) rowY = yBright;
#endif
    int t0, t1, ty, th; sliderTrack(colX, rowY, colW, sH, &t0, &t1, &ty, &th);
    float f = (float)((int)lx - t0) / (float)(t1 - t0);
    if (f < 0) f = 0; if (f > 1) f = 1;
    if      (gDrag == 1) { cfg.soundVolume = (uint8_t)(f * 100 + 0.5f); }
    else if (gDrag == 2) { cfg.flapMs      = (uint16_t)(2 + f * 498); }
#if SETTINGS_HAS_BRIGHTNESS
    else if (gDrag == 0) { cfg.panelBright = (uint8_t)(1 + f * 254); panelSetBrightness(cfg.panelBright); }
#endif
    gCfgDirty = true;
  }

  // ---- taps: buttons + toggles (disabled while animating in) ----
  if (tap && !animating) {
    if (inRect(tpx, tpy, doneX, doneY, doneW, doneH)) { settingsClose(); return false; }
    else if (inRect(tpx, tpy, rbX, rbY, rbW, rbH)) {                        // Reboot
      if (gCfgDirty) saveConfig();
      panelFillRect(0, 0, W, H, 16, 18, 24);
      sTxt(W / 2, H / 2 - titleSz / 2, titleSz, "Rebooting...", 235, 238, 245, 1);
      panelShow(); vTaskDelay(pdMS_TO_TICKS(600));
      ESP.restart();
    }
    else if (inRect(tpx, tpy, colX, yTgl, halfW, sH)) {                     // Quiet Time
      sfSetQuietTime(!gQuietTime); gCfgDirty = true;
    }
    else if (inRect(tpx, tpy, colX + halfW + pad, yTgl, halfW, sH)) {       // Sound
      cfg.soundEnabled = !cfg.soundEnabled; if (!cfg.soundEnabled) soundStop(); gCfgDirty = true;
    }
    // Schedule rows: tap the pill (right end) toggles enable; tap elsewhere on the row opens the
    // time editor.
    else if (inRect(tpx, tpy, colX, ySchQ, colW, schH)) {
      if (tpx >= colX + colW - schPillW) { cfg.quietSchedEnabled = !cfg.quietSchedEnabled; gCfgDirty = true; }
      else gEditSched = 0;
    }
#if SETTINGS_HAS_BRIGHTNESS
    else if (inRect(tpx, tpy, colX, ySchD, colW, schH)) {
      if (tpx >= colX + colW - schPillW) { cfg.dimEnabled = !cfg.dimEnabled; gCfgDirty = true; }
      else gEditSched = 1;
    }
#endif
  }

  // ---- draw ----
  panelFillRect(0, 0, W, H, 16, 18, 24);              // full bg (no yOff -> no stale during slide)
  panelFillRect(0, yOff, W, hdrH, 30, 34, 46);        // header bar (slides)
  panelFillRect(0, hdrH + yOff, W, 2, 70, 80, 110);
  sTxt(pad, (hdrH - titleSz) / 2 + yOff, titleSz, "Settings", 235, 238, 245, 0);
  drawButton(doneX, doneY, doneW, doneH, "Done", 60, 68, 88);

  char v[24];
#if SETTINGS_HAS_BRIGHTNESS
  snprintf(v, sizeof(v), "%d%%", cfg.panelBright * 100 / 255);
  drawSlider(colX, yBright, colW, sH, "Brightness", (cfg.panelBright - 1) / 254.0f, v);
#endif
  snprintf(v, sizeof(v), "%d%%", cfg.soundVolume);
  drawSlider(colX, yVol, colW, sH, "Volume", cfg.soundVolume / 100.0f, v);
  snprintf(v, sizeof(v), "%ums", cfg.flapMs);
  drawSlider(colX, yFlip, colW, sH, "Flip step", (cfg.flapMs - 2) / 498.0f, v);

  drawToggle(colX, yTgl, halfW, sH, "Quiet Time", gQuietTime, nullptr);
  drawToggle(colX + halfW + pad, yTgl, halfW, sH, "Sound", cfg.soundEnabled, nullptr);

  char sub[48];
  snprintf(sub, sizeof(sub), "%s - %s    tap to edit", cfg.quietStart, cfg.quietEnd);
  drawToggle(colX, ySchQ, colW, schH, "Quiet schedule", cfg.quietSchedEnabled, sub);
#if SETTINGS_HAS_BRIGHTNESS
  snprintf(sub, sizeof(sub), "%s - %s    dim %d%%    tap to edit", cfg.dimStart, cfg.dimEnd, cfg.dimLevel * 100 / 255);
  drawToggle(colX, ySchD, colW, schH, "Dim schedule", cfg.dimEnabled, sub);
#endif

  // ---- info block ----
  IPAddress ip = WiFi.localIP();
#if BOARD_HAS_ETH
  if ((uint32_t)ip == 0) ip = ETH.localIP();
#endif
  const int isz = (int)(sH * 0.34f);
  char line[140];
  snprintf(line, sizeof(line), "%s    %u.%u.%u.%u    %s %ddBm",
           cfgHostname(), ip[0], ip[1], ip[2], ip[3], WiFi.SSID().c_str(), (int)WiFi.RSSI());
  sTxt(colX, yInfo, isz, line, 150, 175, 205, 0);
  snprintf(line, sizeof(line), "fw %s    %s    %dx%d    %dx%d flaps",
           FW_VERSION, BOARD_ID_STR, W, H, (int)gPanel.cols, (int)gPanel.rows);
  sTxt(colX, yInfo + isz + 6, isz, line, 150, 175, 205, 0);

  drawButton(rbX, rbY, rbW, rbH, "Reboot", 150, 60, 60);

  panelShow();
  vTaskDelay(pdMS_TO_TICKS(animating ? 16 : 30));    // ~60fps while sliding, relaxed once settled
  return true;
}
