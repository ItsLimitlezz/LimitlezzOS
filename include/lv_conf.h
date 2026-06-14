/**
 * LimitlezzOS lv_conf.h — LVGL 8.3, stripped per master spec §4.4:
 * no theme system, no image decoders, no unused widgets, baked fonts only.
 * Anything not defined here falls back to lv_conf_internal.h defaults.
 */
#if 1 /* Set it to "1" to enable content */

#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (96U * 1024U)

#define LV_DISP_DEF_REFR_PERIOD 30
#define LV_INDEV_DEF_READ_PERIOD 30

/* Tick from the platform (millis() on T-Deck, SDL_GetTicks() in sim) */
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "lz_tick.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (lz_tick_ms())

#define LV_DPI_DEF 130

/* ---- Features stripped (spec: minimum RAM/flash for OS shell) ---- */
#define LV_USE_THEME_DEFAULT 0
#define LV_USE_THEME_BASIC 0
#define LV_USE_THEME_MONO 0
#define LV_USE_PNG 0
#define LV_USE_BMP 0
#define LV_USE_SJPG 0
#define LV_USE_GIF 0
#define LV_USE_QRCODE 0
#define LV_USE_FREETYPE 0
#define LV_USE_FFMPEG 0

/* Widgets: keep only what the shell draws with */
#define LV_USE_ARC 1        /* battery ring on System screen */
#define LV_USE_BAR 0
#define LV_USE_BTN 0
#define LV_USE_BTNMATRIX 0
#define LV_USE_CANVAS 0
#define LV_USE_CHECKBOX 0
#define LV_USE_DROPDOWN 0
#define LV_USE_IMG 1        /* needed by snapshot */
#define LV_USE_LABEL 1
#define LV_USE_LINE 1       /* composer send-arrow glyph */
#define LV_USE_ROLLER 0
#define LV_USE_SLIDER 0
#define LV_USE_SWITCH 0
#define LV_USE_TEXTAREA 0
#define LV_USE_TABLE 0
#define LV_USE_ANIMIMG 0
#define LV_USE_CALENDAR 0
#define LV_USE_CHART 0
#define LV_USE_COLORWHEEL 0
#define LV_USE_IMGBTN 0
#define LV_USE_KEYBOARD 0
#define LV_USE_LED 0
#define LV_USE_LIST 0
#define LV_USE_MENU 0
#define LV_USE_METER 0
#define LV_USE_MSGBOX 0
#define LV_USE_SPAN 0
#define LV_USE_SPINBOX 0
#define LV_USE_SPINNER 0
#define LV_USE_TABVIEW 0
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN 0

#define LV_USE_SNAPSHOT 1   /* sim screenshot verification */
#define LV_USE_GRID 0
#define LV_USE_FLEX 1

/* ---- Fonts: baked tables in flash, no runtime loading ---- */
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_UNSCII_8 1
#define LV_FONT_DEFAULT &lv_font_montserrat_12
#define LV_USE_FONT_COMPRESSED 0

#endif /* LV_CONF_H */
#endif /* End of "Content enable" */
