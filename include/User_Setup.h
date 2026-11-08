#pragma once

// Force-included by PlatformIO so TFT_eSPI's own translation units receive
// exactly the same driver and pin definitions as the application.
#define USER_SETUP_LOADED

#ifdef BOARD_ESP32_3248S035C
// Sunton's onboard ST7796 uses HSPI pins, not the microSD's VSPI pins.
#define ST7796_DRIVER
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC   2
#define TFT_RST  -1
#define TFT_BL   27
#define TFT_BACKLIGHT_ON 1
#define TFT_RGB_ORDER TFT_BGR
#define TFT_INVERSION_OFF
#define USE_HSPI_PORT
// The ESP32-3248S035C BSP uses 80 MHz on the onboard ST7796/HSPI bus.
// Override with -DSPI_FREQUENCY=40000000 if a panel needs the slower clock.
#ifndef SPI_FREQUENCY
#define SPI_FREQUENCY 80000000
#endif
#else
// External BTT TFT35-SPI V2.1 (ILI9488).
#define TFT_MISO 19
#define TFT_MOSI 23
#define TFT_SCLK 18
#define TFT_CS   5
#define TFT_DC   15
#define TFT_RST  -1
#define TFT_BL   32
#define ILI9488_DRIVER
#ifndef SPI_FREQUENCY
#define SPI_FREQUENCY 20000000
#endif
#endif

#define LOAD_GLCD
// LVGL draws the interface with display_fonts.cpp. TFT_eSPI only transfers
// its pixel buffers; keep the small GLCD fallback without duplicate fonts.

#define SMOOTH_FONT

#define SPI_READ_FREQUENCY  16000000
#define SPI_TOUCH_FREQUENCY  2500000

#define SUPPORT_TRANSACTIONS
