// display.h -- the flap wall: geometry, the flap renderer, and the reel task.
//
// Output goes through panel.h. Nothing in this file knows a pixel format or a
// brightness value -- see panel.h for why that seam is where it is.
//
// Geometry falls out of the fixed panel size (1280x800) and the module grid:
// a flap card keeps a 1:FLAP_ASPECT proportion, whichever axis constrains sets the
// card size, and the wall is centred on both axes (short walls letterbox/pillarbox).
// The renderer then picks the largest Helvetica flap face that fits the cell
// (fontflap.h), falling back to the small CP1252 bitmap faces for tiny cells.
//
// Each cell draws one flap face. A flap is either a glyph or, at reel indices
// [colourBase, colourBase+colourCount), a solid colour swatch. On the LCD a cell is
// big enough to draw the flap CARD -- a dark card face with a permanent split seam;
// mid-flip the incoming flap's top half sits above the outgoing flap's bottom half,
// split by a fold, exactly as a real split-flap module reads.
//
// The framebuffer lives in PSRAM (panel.cpp allocates MALLOC_CAP_SPIRAM): the DSI
// scanout is fed by the DPI engine, not a CPU/GDMA stream out of internal SRAM, so
// the S3's internal-only constraint does not apply on the P4.

#ifndef MPGW_DISPLAY_H
#define MPGW_DISPLAY_H

#include "common.h"
#include "font1252.h"

// Resolved once at boot from cfg. Reported by GET /api/status so the UI can show
// what the firmware actually decided.
struct PanelGeometry {
  uint16_t panelW, panelH;
  uint8_t  cols, rows;
  uint16_t cellW, cellH;         // u16: an 800px panel makes 266px cells (u8 wrapped -- found the hard way)
  uint16_t originX, originY;     // centring margin (rows centre vertically: flap cards
                                 // keep their aspect, so short walls letterbox)
  const struct FlapFace* flap;   // Helvetica flap face for these cells, or null (bitmap faces)
  const Font1252* font;
  bool     ready;                // the DMA driver started successfully
};

extern PanelGeometry gPanel;

// Compute the grid for a panel + module grid, clamping anything unusable. Pure --
// no hardware touched -- so config validation can call it before committing.
PanelGeometry dispPlan(uint16_t panelW, uint16_t panelH, uint8_t cols, uint8_t rows);

// Construct and start the panel driver from cfg. Safe to call once, from setup().
// Logs and leaves gPanel.ready false if the panel cannot start; the gateway then
// runs headless rather than refusing to boot.
void dispInit();

// Redraw every cell from the modules' current reel state and push a frame. Returns
// false without painting if the panel is not up or the module lock was busy -- the
// caller should leave its repaint pending and try again rather than drop the frame.
bool dispRender();

// Draw a geometry probe (border, diagonal, chain seam, RGB corners) for four seconds.
// Called from dispInit() when PANEL_BOOT_TEST is 1. Panel coordinates, no wall, no font.
void dispTestPattern();

// Force a redraw on the next display-task iteration (config change, boot).
void dispMarkDirty();

// Drop any running/pending effect and release the raw canvas, then repaint the reel wall. Safe
// from any task. Called when a split-flap command arrives, on canvas release, and by Quiet Time.
void dispReturnToWall();
// True while the panel is showing PIXELS rather than the flap wall -- raw canvas, an
// effect, an on-device animation or a ticker. The display-state JSON reports it as
// "mode":"pixels" so the dashboard preview knows to render the framebuffer readback
// instead of flap cells (v3.0.1).
bool dispPixelsMode();

// Blit font rows [rowFrom,rowTo) of the glyph for CP1252 byte `ch` at (px,py),
// solid colour, bit 15 = leftmost column. The one shared glyph blitter -- the
// ticker (canvas.cpp), the effects (fliporama/clock) and the canvas text op all
// draw through it. Rows clamp to the face height, so (0, 255) means "whole glyph".
void dispDrawGlyph1252(int px, int py, const Font1252* f, uint8_t ch,
                       int rowFrom, int rowTo, uint8_t r, uint8_t g, uint8_t b);

// Show black and halt the panel's GDMA output for the duration of an OTA upload.
// Reversible -- a failed upload calls dispResume().
void dispBlank();

// Restart the panel output and repaint. Pairs with dispBlank().
void dispResume();

// Black, halted, and marked not-ready. Call only once a reboot is certain.
void dispStop();

// Human-readable face name for the status page, e.g. "6x9".
const char* dispFontName();

#endif // MPGW_DISPLAY_H
