// board_p4_lcd_7b.h -- board profile: Waveshare ESP32-P4-WIFI6-Touch-LCD-7B + 7" DSI panel.
//
// PHASE-1 SCAFFOLD. This documents the target board so the profile layer is complete, but
// the env is NOT yet buildable: it needs the EK79007 driver component and the direct-present
// path (Phase 2), and the values tagged TODO need Phase-0 confirmation from Waveshare's
// ESP-IDF demo / schematic. A leaf header of #defines ONLY. See BOARD_SUPPORT_PLAN.md.
#ifndef LCDGW_BOARD_P4_LCD_7B_H
#define LCDGW_BOARD_P4_LCD_7B_H

// ---- identity ----
#define BOARD_ID_STR          "waveshare-p4-lcd-7b"
#define BOARD_DESC_STR        "Waveshare ESP32-P4-WIFI6-Touch-LCD-7B + 7\" DSI (EK79007)"

// ---- panel: driver + geometry ----
#define PANEL_DRIVER_JD9365   0
#define PANEL_DRIVER_EK79007  1        // espressif/esp_lcd_ek79007 managed component (add in Phase 2)
#define PANEL_NATIVE_W        1024     // panel is 1024x600 NATIVE LANDSCAPE -- no rotation
#define PANEL_NATIVE_H        600
#define PANEL_ROT_180         0
#define PANEL_ROTATE_DEG      0        // direct present (Phase 2 rotation-optional path)
#define DEFAULT_PANEL_W       1024     // logical == native (no rotate)
#define DEFAULT_PANEL_H       600
#define PANEL_MAX_W           1024
#define EFFECT_RENDER_SCALE   4        // 1024 / 4 = 256 px effect surface

// ---- MIPI-DSI ----
#define PANEL_DSI_LANES       2        // TODO confirm (EK79007 is commonly 2-lane)
#define PANEL_DSI_LANE_MBPS   1000     // TODO confirm from the EK79007 component defaults
#define MIPI_LDO_CHAN         3        // TODO confirm the D-PHY LDO channel on this board
#define MIPI_LDO_MV           2500
#define DSI_RESET_PIN         -1       // TODO confirm from the schematic
#define LCD_BL_I2C_ADDR       -1       // TODO backlight method (PWM pin vs I2C chip) -- Phase 0

// ---- shared I2C (touch + codec) ----
#define I2C_SDA_PIN           7        // TODO confirm the 7B touch/codec I2C pins
#define I2C_SCL_PIN           8

// ---- capability flags ----
#define BOARD_HAS_ETH         0        // WiFi-only (no Ethernet PHY) -- ETH init compiled out
#define BOARD_HAS_RTC         1        // RTC + 1220 battery holder present (Phase 4)
#define BOARD_HAS_IMU         0
#define BOARD_AUDIO_ES8311    1        // playback + codec control
#define BOARD_AUDIO_ES7210    1        // dedicated dual-mic capture (echo-cancel) -- Phase 4

#endif // LCDGW_BOARD_P4_LCD_7B_H
