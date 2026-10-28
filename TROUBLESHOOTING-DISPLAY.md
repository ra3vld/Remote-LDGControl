# Display Troubleshooting — BTT TFT35-SPI V2.1

## Symptoms

On NodeMCU ESP32S v1.1 with BTT TFT35-SPI V2.1 and the `esp32-display`
firmware, the backlight comes on but the screen stays gray or white without
the LVGL interface.

## Hardware

Source: [BTT hardware files](https://github.com/bigtreetech/TFT35-SPI/tree/master/v2/Hardware),
specifically `BTT TFT35-SPI V2.1_IO.pdf` and `BTT TFT35-SPI V2.1_SCH.pdf`.

The BTT TFT35-SPI V2.1 connects the ILI9488 LCD and NS2009 touch controller
directly to J1 over SPI and I2C. It has no onboard MCU.

J1 (XH 2.54 mm, 10-pin) pinout:

| J1 pin | Signal | Notes |
|---|---|---|
| 1 | IORQ (NS2009 PENIRQ) | Touch interrupt |
| 2 | SCL (NS2009) | I2C clock |
| 3 | SDA (NS2009) | I2C data |
| 4 | RS (LCD)  | Data/command |
| 5 | NSS (LCD) | Chip select |
| 6 | SCK (LCD) | SPI clock |
| 7 | MOSI (LCD) | |
| 8 | MISO (LCD) | |
| 9 | GND | |
| 10 | +5V | Board has on-board AMS1117-3.3 |

J1 has no RST or BL pin. The LCD reset input is pulled up to the onboard
3.3 V rail through R10 (10 kΩ). The backlight is powered through a
current-limiting resistor and stays on while 5 V is supplied.
See [WIRING.md](WIRING.md#03-внешний-дисплей-btt--только-nodemcu) for ESP32 connections.

## Configuration

The BTT branch of `include/User_Setup.h` selects `ILI9488_DRIVER`,
`TFT_RST = -1` and `SPI_FREQUENCY = 20000000` (20 MHz).
The configured `TFT_BL = 32` has no connection through J1.
TFT_eSPI sends a software reset (`0x01`) during ILI9488 initialization.

## Diagnostic steps

1. Check continuity between each ESP32 GPIO and J1 using the wiring table.
   Check the connector crimps, common ground, 5 V input and onboard 3.3 V rail.
2. Build and flash the `esp32-display` target. Capture the console with
   `pio device monitor -b 115200`. Check whether startup reaches
   `display.begin()` and whether any panics or brownouts occur.
3. To check LCD communication before LVGL starts, temporarily add the
   following after `m_tft->init()` and `setRotation()` in `DisplayUI::begin()`:

   ```cpp
   m_tft->fillScreen(TFT_RED);
   delay(500);
   ```

4. Use the result to choose the next check:

   | Result | Next check |
   |---|---|
   | Red flash, then LVGL interface | Remove the diagnostic fill |
   | Red flash, then gray screen | Check `displayFlush`, draw buffers and rotation |
   | No red flash | Check SPI wiring, chip select and initialization |

5. If the screen stays gray, try shorter SPI wires, a delay before display
   initialization, another ESP32 board, or an ILI9488 example from TFT_eSPI.
   Remove the diagnostic fill after troubleshooting.

## Reference files

- `platformio.ini` — `esp32-display` env, `build_flags` for TFT_eSPI.
- `include/User_Setup.h` — TFT_eSPI pin configuration.
- `include/config.h` — display pin constants (`TFT_*`, `TOUCH_*`, `DISPLAY_*`).
- `src/display_ui.cpp` — `DisplayUI::begin()` is where `m_tft->init()` happens.
- `WIRING.md` — ESP32 and J1 wiring table.
