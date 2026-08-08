// board_p4_lcd_10.h -- board profile: Waveshare ESP32-P4-WIFI6-POE-ETH + 10.1" DSI panel.
//
// The shipping LCD Gateway board. A leaf header of #defines ONLY (no includes, no types),
// selected by src/board.h from the -DBOARD_P4_LCD_10 build macro. Everything board-specific
// lives here so the rest of the tree is panel-agnostic. See BOARD_SUPPORT_PLAN.md.
#ifndef LCDGW_BOARD_P4_LCD_10_H
#define LCDGW_BOARD_P4_LCD_10_H

// ---- identity ----
#define BOARD_ID_STR          "waveshare-p4-lcd-10"
#define BOARD_DESC_STR        "Waveshare ESP32-P4-WIFI6-POE-ETH + 10.1\" DSI (JD9365)"

// ---- panel: driver + geometry ----
#define PANEL_DRIVER_JD9365   1        // the vendored JD9365 esp_lcd driver (panel.cpp)
#define PANEL_DRIVER_EK79007  0
#define PANEL_NATIVE_W        800      // physical panel is 800x1280 PORTRAIT
#define PANEL_NATIVE_H        1280
#define PANEL_ROT_180         0        // 1 if the landscape mount is upside-down
#define PANEL_ROTATE_DEG      90       // logical landscape -> native portrait, via the PPA
#define DEFAULT_PANEL_W       1280     // landscape logical width  (= native H, rotated)
#define DEFAULT_PANEL_H       800      // landscape logical height (= native W, rotated)
#define PANEL_MAX_W           1280     // width of the static row buffers (canvas/effects/web)
#define EFFECT_RENDER_SCALE   5        // logical_W / 5 = 256 px effect surface (PPA scales up)

// ---- MIPI-DSI ----
#define PANEL_DSI_LANES       2
#define PANEL_DSI_LANE_MBPS   1500
#define MIPI_LDO_CHAN         3        // P4 D-PHY power rail (2.5 V on LDO channel 3)
#define MIPI_LDO_MV           2500
#define DSI_RESET_PIN         -1       // panel reset GPIO (-1 = none / not routed)
#define LCD_BL_I2C_ADDR       0x45     // backlight/power controller on the FPC (regs 0x95/0x96)

// ---- shared I2C (display backlight ctrl, GT9xx touch, ES8311 codec ctrl) ----
#define I2C_SDA_PIN           7
#define I2C_SCL_PIN           8

// ---- capability flags (compile-time; drive #if gates + /api/config board reporting) ----
#define BOARD_HAS_ETH         1        // IP101 Ethernet PHY + PoE (RMII pins in platformio.ini)
#define BOARD_HAS_RTC         0        // no RTC chip -- NTP-only clock (rtc.cpp handles it)
#define BOARD_HAS_IMU         0        // no IMU -- the touch panel replaces tap gestures
#define BOARD_AUDIO_ES8311    1        // mic capture + playback via the ES8311 codec
#define BOARD_AUDIO_ES7210    0        // no dedicated capture codec

#endif // LCDGW_BOARD_P4_LCD_10_H
