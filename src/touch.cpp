#include "common.h"
#include "touch.h"
#include <Wire.h>

// touch.cpp -- see touch.h. GT911 over the shared Wire bus, no INT/RST wired, so we
// poll. All runtime I2C is on taskRTC (touchTick); the published event flag is drained
// lock-free from taskWeb (touchPoll), the same shape as imu.cpp's tap engine.
//
// GT911 register map (16-bit big-endian addresses):
//   0x8140  product id, 4 bytes ("911" ascii)
//   0x814E  status: bit7 = a fresh touch buffer is ready, bits3:0 = point count (0..5)
//   0x8150  point 1 coords, LE16 pairs from byte 0: [x_lo,x_hi, y_lo,y_hi, size...]
// After reading a buffer we MUST write 0 to 0x814E, or the controller never refreshes.

static const uint8_t  GT_ADDR_MAIN = 0x5D;
static const uint8_t  GT_ADDR_ALT  = 0x14;
static const uint16_t REG_STATUS   = 0x814E;
static const uint16_t REG_POINT1   = 0x8150;
static const uint16_t REG_PRODUCT  = 0x8140;

static uint8_t  gAddr    = 0;                 // 0 until probed OK
static bool     gDown    = false;            // a finger is currently on the glass
static uint32_t gLastTapMs = 0;              // refractory
static uint32_t gLastTouchMs = 0;            // last tick a finger was seen (lift-by-timeout)
static uint16_t gLastX = 0, gLastY = 0;

static portMUX_TYPE touchMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t  gPending = 0;        // 1 = a tap event waiting to be drained
static volatile uint32_t gSeq = 0;
static volatile uint32_t gTotal = 0;
static volatile uint16_t gPubX = 0, gPubY = 0;
static volatile uint16_t gPubNx = 0, gPubNy = 0;   // raw GT911 coords (pre-toLogical), for calibration

// Swipe-down-from-top-edge -> opens the on-device settings shade (settings_ui.cpp), tracked
// here on taskRTC; the one-shot gSwipeDown is consumed by touchSwipeDown() from taskDisplay.
static uint16_t gSwStartY = 0;
static bool     gSwActive = false;          // a top-edge press is in progress
static volatile bool gSwipeDown = false;    // one-shot: a completed downward swipe
#define TOUCH_SWIPE_TOP_EDGE  60            // the press must land within this many px of the top
#define TOUCH_SWIPE_MIN_DIST  110           // and drag down at least this far to fire

// UI tap latch: a stationary press+release (NOT a drag/swipe), captured HERE in touchTick so a
// slow UI render loop can't miss the brief down phase. Consumed by touchTapConsume(); the point
// reported is the press location. This is what the settings shade hit-tests buttons/toggles on.
static uint16_t gTapDownX = 0, gTapDownY = 0;
static bool     gTapMoved = false;          // this press has moved too far to be a tap (a drag)
static volatile bool     gUiTap  = false;   // one-shot: a completed tap
static volatile uint16_t gUiTapX = 0, gUiTapY = 0;
#define TOUCH_TAP_MOVE_TOL  28             // a press may jitter this far and still count as a tap
#define TOUCH_LIFT_MS       70             // no fresh touch for this long = the finger has lifted

// ---- raw register I/O (Wire; caller ensures single-threaded / taskRTC) -------------
static bool gtRead(uint16_t reg, uint8_t* buf, size_t n) {
  Wire.beginTransmission(gAddr);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  if (Wire.endTransmission(false) != 0) return false;   // repeated start, keep the bus
  const size_t got = Wire.requestFrom((int)gAddr, (int)n);
  if (got != n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}
static bool gtWrite8(uint16_t reg, uint8_t val) {
  Wire.beginTransmission(gAddr);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  Wire.write(val);
  return Wire.endTransmission(true) == 0;
}

void touchInit() {
  for (uint8_t a : {GT_ADDR_MAIN, GT_ADDR_ALT}) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission(true) != 0) continue;
    gAddr = a;
    uint8_t id[4] = {0};
    gtRead(REG_PRODUCT, id, 4);                          // "911" on this panel
    printf("[TOUCH] GT911 at 0x%02x (id %c%c%c%c)\n", a,
           id[0] ? id[0] : '?', id[1] ? id[1] : '?', id[2] ? id[2] : '?', id[3] ? id[3] : ' ');
    return;
  }
  printf("[TOUCH] no GT911 on I2C 0x5D/0x14 -- touch disabled\n");
}

bool touchAvailable() { return gAddr != 0; }

// The GT911 reports in the panel's native PORTRAIT frame: nx is the 800-axis, ny the
// 1280-axis. Mapped to the landscape mount so (0,0) is top-left and (W-1,H-1) is
// bottom-right -- derived empirically by touching the four corners. If a rebuild ever
// mounts the panel the other way round, these two lines are the one place to flip.
static inline void toLogical(uint16_t nx, uint16_t ny, uint16_t* lx, uint16_t* ly) {
#if PANEL_ROTATE_DEG == 0
  // Native-landscape board (7B, EK79007): the GT911 sits rotated 180 from the display, so BOTH
  // axes run opposite it -- physical top-left reads raw ~(W,H), bottom-right ~(0,0). Verified by
  // touching all four corners (TL raw(960,527) .. BR raw(71,48) on 1024x600). Invert both; nx is
  // the width axis, ny the height axis.
  *lx = (nx < DEFAULT_PANEL_W) ? (uint16_t)(DEFAULT_PANEL_W - 1 - nx) : 0;
  *ly = (ny < DEFAULT_PANEL_H) ? (uint16_t)(DEFAULT_PANEL_H - 1 - ny) : 0;
#else
  // 10.1" portrait panel mounted landscape (rotated 90): the GT911 reports in the panel's
  // native PORTRAIT frame (nx the 800-axis, ny the 1280-axis); rotate it onto the mount.
  *lx = (ny < DEFAULT_PANEL_W) ? (uint16_t)(DEFAULT_PANEL_W - 1 - ny) : 0;
  *ly = (nx < DEFAULT_PANEL_H) ? nx : (uint16_t)(DEFAULT_PANEL_H - 1);
#endif
}

void touchTick() {
  if (!gAddr) return;
  uint8_t st;
  if (!gtRead(REG_STATUS, &st, 1)) return;
  const uint32_t now = millis();
  bool released = false;
  if (st & 0x80) {                                        // a fresh touch buffer is ready
    const uint8_t pts = st & 0x0F;
    if (pts) {
      uint8_t p[8];
      if (gtRead(REG_POINT1, p, 8)) {
        const uint16_t nx = (uint16_t)(p[0] | (p[1] << 8));   // panel 800-axis
        const uint16_t ny = (uint16_t)(p[2] | (p[3] << 8));   // panel 1280-axis
        uint16_t lx, ly; toLogical(nx, ny, &lx, &ly);
        gLastX = lx; gLastY = ly; gLastTouchMs = now;
        const bool pressEdge = !gDown;
        if (pressEdge) {
          // New press: seed BOTH the swipe-open candidate and the UI-tap candidate.
          gSwStartY = ly; gSwActive = (ly < TOUCH_SWIPE_TOP_EDGE);
          gTapDownX = lx; gTapDownY = ly; gTapMoved = false;
        } else {
          // Swipe-down-from-top-edge -> open the settings shade (fires once past the threshold).
          if (gSwActive && (int)ly - (int)gSwStartY >= TOUCH_SWIPE_MIN_DIST) {
            gSwipeDown = true; gSwActive = false;
          }
          // Real movement disqualifies this press from being a tap (it's a drag/swipe).
          if (abs((int)lx - (int)gTapDownX) > TOUCH_TAP_MOVE_TOL ||
              abs((int)ly - (int)gTapDownY) > TOUCH_TAP_MOVE_TOL) gTapMoved = true;
        }
        // A press EDGE is the tap moment for the SSE gesture (immediate, responsive for dismiss);
        // 120 ms refractory debounces the jitter. A drag still counts as one edge = one tap.
        if (pressEdge && (uint32_t)(now - gLastTapMs) > 120) {
          gLastTapMs = now;
          taskENTER_CRITICAL(&touchMux);
          gPending = 1; gSeq = gSeq + 1; gTotal = gTotal + 1;
          taskEXIT_CRITICAL(&touchMux);
        }
        // Publish the LIVE finger position every tick while down (was: only on the press edge)
        // so the settings shade can drive slider drags + taps; /api/gestures shows it too.
        taskENTER_CRITICAL(&touchMux);
        gPubX = lx; gPubY = ly; gPubNx = nx; gPubNy = ny;
        taskEXIT_CRITICAL(&touchMux);
        gDown = true;
      }
    } else {
      released = true;                                    // explicit pts==0 release report
    }
    gtWrite8(REG_STATUS, 0);                              // ack: let the controller refresh
  }
  // Finger LIFT = an explicit release OR no fresh touch for TOUCH_LIFT_MS. The GT911 on this
  // panel does NOT reliably emit the pts==0 release when polled, so the timeout is the real
  // lift detector -- and the one thing that makes a press+release reliably land as a tap.
  // A stationary press that lifts is a UI tap; a moved one was a drag/swipe (excluded).
  if (gDown && (released || (uint32_t)(now - gLastTouchMs) > TOUCH_LIFT_MS)) {
    if (!gTapMoved) {
      taskENTER_CRITICAL(&touchMux);
      gUiTap = true; gUiTapX = gTapDownX; gUiTapY = gTapDownY;
      taskEXIT_CRITICAL(&touchMux);
    }
    gDown = false; gSwActive = false;
  }
}

bool touchPoll(uint8_t* countOut, uint32_t* seqOut) {
  bool has = false;
  taskENTER_CRITICAL(&touchMux);
  if (gPending) {
    if (countOut) *countOut = gPending;
    if (seqOut)   *seqOut   = gSeq;
    gPending = 0;
    has = true;
  }
  taskEXIT_CRITICAL(&touchMux);
  return has;
}

uint32_t touchTotal() { return gTotal; }

bool touchPoint(uint16_t* x, uint16_t* y) {
  if (x) *x = gPubX;
  if (y) *y = gPubY;
  return gDown;
}

bool touchSwipeDown() {
  if (!gSwipeDown) return false;
  gSwipeDown = false;                        // one-shot
  return true;
}

void touchRawPoint(uint16_t* nx, uint16_t* ny) {
  if (nx) *nx = gPubNx;
  if (ny) *ny = gPubNy;
}

bool touchTapConsume(uint16_t* x, uint16_t* y) {
  bool has = false;
  taskENTER_CRITICAL(&touchMux);
  if (gUiTap) { if (x) *x = gUiTapX; if (y) *y = gUiTapY; gUiTap = false; has = true; }
  taskEXIT_CRITICAL(&touchMux);
  return has;
}
