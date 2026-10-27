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
#define SPI_FREQUENCY 40000000
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
#define SPI_FREQUENCY 20000000
#endif

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8

#define SMOOTH_FONT

#define SPI_READ_FREQUENCY  16000000
#define SPI_TOUCH_FREQUENCY  2500000

#define SUPPORT_TRANSACTIONS
