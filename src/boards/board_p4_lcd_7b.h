// board_p4_lcd_7b.h -- board profile: Waveshare ESP32-P4-WIFI6-Touch-LCD-7B + 7" DSI panel.
//
// PHASE-2 (bring-up in progress). Display specs below are CONFIRMED from Waveshare's own
// EK79007 example (waveshareteam/ESP32-P4-WIFI6-Touch-LCD-7B, 07_color_panel). Non-display
// pins (touch/SD/audio) are still TODO -- they come from the i2c_tools/sdmmc/codec examples
// and are only needed after the display lights up (Phase 3). A leaf header of #defines ONLY.
// See BOARD_SUPPORT_PLAN.md.
#ifndef LCDGW_BOARD_P4_LCD_7B_H
#define LCDGW_BOARD_P4_LCD_7B_H

// ---- identity ----
#define BOARD_ID_STR          "waveshare-p4-lcd-7b"
#define BOARD_DESC_STR        "Waveshare ESP32-P4-WIFI6-Touch-LCD-7B + 7\" DSI (EK79007)"

// ---- panel: driver + geometry (CONFIRMED) ----
#define PANEL_DRIVER_JD9365   0
#define PANEL_DRIVER_EK79007  1        // esp_lcd_new_panel_ek79007() -- vendored in Phase 2
#define PANEL_NATIVE_W        1024     // 1024x600 NATIVE LANDSCAPE -- no rotation
#define PANEL_NATIVE_H        600
#define PANEL_ROT_180         0
#define PANEL_ROTATE_DEG      0        // direct present (no PPA rotate) -- Phase 2 path
#define DEFAULT_PANEL_W       1024     // logical == native
#define DEFAULT_PANEL_H       600
#define PANEL_MAX_W           1024
#define EFFECT_RENDER_SCALE   4        // 1024 / 4 = 256 px effect surface width

// ---- MIPI-DSI (CONFIRMED from 07_color_panel + esp_lcd_ek79007.h) ----
// panel.cpp uses the component's EK79007_1024_600_PANEL_60HZ_CONFIG macro for the DPI timing
// (dpi_clk 52 MHz; hsync 10/160/160; vsync 1/23/12), so those are not repeated here.
#define PANEL_DSI_LANES       2
#define PANEL_DSI_LANE_MBPS   900       // EK79007_PANEL_BUS_DSI_2CH_CONFIG default
#define MIPI_LDO_CHAN         3         // same D-PHY rail as the 10.1" board
#define MIPI_LDO_MV           2500
#define DSI_RESET_PIN         33        // EK79007 reset GPIO
#define LCD_BL_I2C_ADDR       -1        // no I2C backlight chip; the example drives BK_LIGHT = -1
                                        // (backlight appears always-on / not SW-controlled here)

// ---- shared I2C (touch + codec) ----
#define I2C_SDA_PIN           7         // TODO confirm the 7B touch/codec I2C pins (03_i2c_tools)
#define I2C_SCL_PIN           8

// ---- capability flags ----
#define BOARD_HAS_ETH         0         // WiFi-only (no Ethernet PHY) -- ETH init compiled out
#define BOARD_HAS_RTC         1         // RTC + 1220 battery holder present (Phase 4)
#define BOARD_HAS_IMU         0
#define BOARD_AUDIO_ES8311    1         // playback + codec control
#define BOARD_AUDIO_ES7210    1         // dedicated dual-mic capture (echo-cancel) -- Phase 4

#endif // LCDGW_BOARD_P4_LCD_7B_H
