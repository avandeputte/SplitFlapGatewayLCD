// board.h -- selects the active board profile from a -DBOARD_* macro in platformio.ini.
//
//   -DBOARD_P4_LCD_10  -> 10.1" JD9365 board  (the shipping board; the default)
//   -DBOARD_P4_LCD_7B  -> 7"    EK79007 board (Phase 2, work in progress)
//
// Each profile under boards/ is a leaf header of #defines only, so this is safe to include
// as early as common.h. One codebase, many P4 display boards. See BOARD_SUPPORT_PLAN.md.
#ifndef LCDGW_BOARD_H
#define LCDGW_BOARD_H

#if defined(BOARD_P4_LCD_7B)
#  include "boards/board_p4_lcd_7b.h"
#elif defined(BOARD_P4_LCD_10)
#  include "boards/board_p4_lcd_10.h"
#else
#  warning "No BOARD_* macro set in platformio.ini -- defaulting to BOARD_P4_LCD_10 (10.1\" JD9365)"
#  include "boards/board_p4_lcd_10.h"
#endif

#endif // LCDGW_BOARD_H
