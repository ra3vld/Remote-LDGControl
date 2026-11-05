#ifndef LV_CONF_H
#define LV_CONF_H

#include <Arduino.h>

// Color depth
#define LV_COLOR_DEPTH 16

// Memory settings
// Full rendering tests peak below 25 KiB even on a 64-bit host. Leave
// internal RAM for Wi-Fi/HTTP/MQTT on the Sunton board without PSRAM.
#define LV_MEM_SIZE (32U * 1024U)
#define LV_MEM_CUSTOM 0

// Tick interface
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

// Display settings
#define LV_DISP_DEF_REFR_PERIOD 30

// Input device settings
#define LV_INDEV_DEF_READ_PERIOD 30

// Feature usage
#define LV_USE_LOG 0
#define LV_USE_LOG_LEVEL LV_LOG_LEVEL_WARN

#define LV_USE_THEME_BASIC 1
#define LV_USE_THEME_MONO 0

// Font usage
#define LV_FONT_DEFAULT &lv_font_montserrat_14
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_48 0

// Widget usage
#define LV_USE_ARC 0
#define LV_USE_BAR 0
#define LV_USE_BTN 1
#define LV_USE_BTNMATRIX 0
#define LV_USE_CANVAS 0
#define LV_USE_CHECKBOX 0
#define LV_USE_DROPDOWN 0
#define LV_USE_IMG 0
#define LV_USE_LABEL 1
#define LV_USE_LINE 1
#define LV_USE_METER 0
#define LV_USE_ROLLER 0
#define LV_USE_SLIDER 0
#define LV_USE_SWITCH 0
#define LV_USE_TEXTAREA 0
#define LV_USE_TABLE 0

// Disable extra widgets
#define LV_USE_ANIMIMG 0
#define LV_USE_CALENDAR 0
#define LV_USE_CHART 0
#define LV_USE_COLORWHEEL 0
#define LV_USE_IMGBTN 0
#define LV_USE_KEYBOARD 0
#define LV_USE_LED 0
#define LV_USE_LIST 0
#define LV_USE_MENU 0
#define LV_USE_MSGBOX 0
#define LV_USE_SPAN 0
#define LV_USE_SPINBOX 0
#define LV_USE_SPINNER 0
#define LV_USE_TABVIEW 0
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN 0

#endif // LV_CONF_H
