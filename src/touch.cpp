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
static uint16_t gLastX = 0, gLastY = 0;

static portMUX_TYPE touchMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t  gPending = 0;        // 1 = a tap event waiting to be drained
static volatile uint32_t gSeq = 0;
static volatile uint32_t gTotal = 0;
static volatile uint16_t gPubX = 0, gPubY = 0;

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
  *lx = (ny < DEFAULT_PANEL_W) ? (uint16_t)(DEFAULT_PANEL_W - 1 - ny) : 0;
  *ly = (nx < DEFAULT_PANEL_H) ? nx : (uint16_t)(DEFAULT_PANEL_H - 1);
}

void touchTick() {
  if (!gAddr) return;
  uint8_t st;
  if (!gtRead(REG_STATUS, &st, 1)) return;
  if (!(st & 0x80)) return;                               // no fresh buffer
  const uint8_t pts = st & 0x0F;
  if (pts) {
    uint8_t p[8];
    if (gtRead(REG_POINT1, p, 8)) {
      const uint16_t nx = (uint16_t)(p[0] | (p[1] << 8));   // panel 800-axis
      const uint16_t ny = (uint16_t)(p[2] | (p[3] << 8));   // panel 1280-axis
      uint16_t lx, ly; toLogical(nx, ny, &lx, &ly);
      gLastX = lx; gLastY = ly;
      // A press EDGE is the tap moment: immediate, responsive for dismiss. A drag is
      // still one edge = one tap. 120 ms refractory debounces the capacitive jitter.
      const uint32_t now = millis();
      if (!gDown && (uint32_t)(now - gLastTapMs) > 120) {
        gLastTapMs = now;
        taskENTER_CRITICAL(&touchMux);
        gPending = 1; gSeq++; gTotal++;
        gPubX = lx; gPubY = ly;
        taskEXIT_CRITICAL(&touchMux);
      }
      gDown = true;
    }
  } else {
    gDown = false;                                        // all fingers lifted
  }
  gtWrite8(REG_STATUS, 0);                                // ack: let the controller refresh
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
