# LDG Tuner Remote Controller

ESP32-based remote control interface for LDG AT-1000ProII / AT-600ProII auto-tuners. Web UI, MQTT, optional touchscreen display, and remote unit support.

## Features

- **Tuner Control**: Full remote control via TTL serial interface
  - Memory Tune, Full Tune, Bypass toggle
  - Auto mode selection
  - Antenna A/B switching
- **Real-time Metering**: Forward power, reflected power, SWR, band detection
- **Web Interface**: Responsive browser UI with Server-Sent Events (SSE) for live updates
- **MQTT Integration**: Telemetry publishing and command subscription
- **Touchscreen Display**: Sunton ESP32-3248S035C or BTT TFT35-SPI with custom cross-needle meter
- **Three Firmware Targets**:
  - `esp32-3248s035c` (default): Sunton 3.5-inch ST7796 + GT911 capacitive touchscreen
  - `esp32-display`: Full unit with touchscreen (shack controller)
  - `esp32-remote`: Headless unit (web UI, MQTT, optional remote client capability)
- **Zero-Config Setup**: WiFi captive portal + web-based configuration — no recompiling needed
- **Security**: HTTP Basic Auth, rate limiting, login lockout

## Hardware

### Required
- Sunton ESP32-3248S035C, or ESP32 WROOM development board for the BTT/headless variants
- 4-pin mini-DIN connector for tuner interface (Kycon KMDLAX-4P)
- **WARNING**: Do NOT use S-Video cables — pins are wired differently and one carries +12V

### Optional (Display Unit)
- BTT TFT35-SPI display (ILI9488 controller)
- NS2009 I2C touchscreen (built into BTT TFT35)
- LM7805 linear regulator + TO-220 heatsink (powers ESP32 + display from 12V supply)

### Wiring

For the Sunton board, follow [ESP32-3248S035C wiring and installation](docs/boards/esp32-3248s035c.md).
Its tuner UART uses **RX GPIO35 / TX GPIO22 on P3**. The table and diagram below apply to the separate ESP32 + BTT hardware.

See [WIRING.md](WIRING.md) for complete details. Quick reference:

![Wiring Diagram](wiring-diagram.svg)

| Connection | ESP32 Pin | Notes |
|------------|-----------|-------|
| Tuner GND | GND | Common ground |
| Tuner TX | GPIO 16 | Tuner TX → ESP32 RX |
| Tuner RX | GPIO 17 | ESP32 TX → Tuner RX |
| 12V Supply | LM7805 IN | Separate 12V supply → 5V regulated |
| Display MOSI | GPIO 23 | Shared SPI bus |
| Display MISO | GPIO 19 | Shared SPI bus |
| Display SCLK | GPIO 18 | Shared SPI bus |
| Display CS | GPIO 5 | Display chip select |
| Display DC | GPIO 15 | Data/command |
| Touch SDA | GPIO 21 | I2C data (NS2009) |
| Touch SCL | GPIO 22 | I2C clock (NS2009) |
| Touch IRQ | GPIO 39 | Touch interrupt |
| Voltage Sense | GPIO 34 | ADC, 100k/22k divider (optional) |

RST and backlight are handled internally by the BTT TFT35-SPI V2.1 board — no external drive lines needed.

**Serial**: 38400 baud, 8N1, TTL (5V). Not RS-232.

## Install

### Option 1: Web Flasher (Recommended)

No software installation required. Works in Chrome, Edge, or Opera.

1. Visit the [Web Flasher](https://ben-kuhn.github.io/Remote-LDGControl/)
2. Connect your ESP32 via USB
3. Select your firmware variant and click **Install**
4. After flashing, the ESP32 creates a `LDGConfig` WiFi AP
5. Connect to it — a captive portal opens for WiFi setup
6. Once connected to your network, access the web UI at the assigned IP

### Option 2: Download Pre-built Firmware

Download the matching image parts from the [latest release](https://github.com/ben-kuhn/Remote-LDGControl/releases).
For Sunton, flash all five parts at their specified offsets:

```bash
esptool.py --chip esp32 --port /dev/ttyUSB0 write_flash \
  --flash_mode dio --flash_freq 40m --flash_size 4MB \
  0x1000 ldg-tuner-3248s035c-bootloader.bin \
  0x8000 ldg-tuner-3248s035c-partitions.bin \
  0xe000 boot_app0.bin \
  0x10000 ldg-tuner-3248s035c.bin \
  0x390000 ldg-tuner-3248s035c-littlefs.bin
```

For the BTT/headless variants, use `display`/`remote` image filenames at the same offsets.

### Option 3: Build from Source

```bash
pip install platformio

pio run -e esp32-display -t upload   # Display unit
pio run -e esp32-remote -t upload    # Remote/headless unit
pio run -e esp32-3248s035c -t upload # Sunton integrated touchscreen (default)
pio run -e esp32-3248s035c -t uploadfs # Web UI in LittleFS (required on first install)
pio device monitor                   # Serial console
```

The build pins PlatformIO Espressif32 6.12.0 (Arduino ESP32 2.0.17), matching the
existing TLS code. Run `pio test -e native` for the GT911 protocol tests.
See the [Sunton board guide](docs/boards/esp32-3248s035c.md) for pins, power,
flash offsets, and checks that still require a physical board.

## Configuration

All settings are configured through the web interface — no need to edit source code or rebuild.

### Initial Setup (Captive Portal)

After first flash, the ESP32 creates an AP named `LDGConfig` (password: `configure`). Connect to it and a captive portal will prompt for your WiFi credentials.

### Web Configuration

Once on your network, go to `http://<esp32-ip>/` and log in (default: `admin` / `change_me_please`).

Go to **Settings** (or `GET /api/config`) to configure:

| Setting | Description |
|---------|-------------|
| WiFi SSID / Password | Your network credentials |
| MQTT Broker | Hostname or IP of your MQTT broker |
| MQTT Port | Broker port (default 1883) |
| MQTT Username / Password | Broker authentication |
| Web Username / Password | Web UI authentication |
| Meter PSU Voltage | Tuner's power supply voltage for accurate power calculation (default 13.8V). This is the voltage at the tuner's 12V input, not the ESP32's supply. |
| Remote Unit ID | ID for remote unit (default 1) |

Settings are saved to NVS and persist across reboots. Changing settings triggers a restart.

### Reset to Defaults

If you need to reconfigure WiFi, press and hold the BOOT button on the ESP32 for 5 seconds, or call `POST /api/config` with `{"reset": true}`.

## Web Interface

Access at `http://<esp32-ip>/` or `http://ldg-tuner-<mac>.local/`

### API Endpoints

All endpoints require HTTP Basic Auth.

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/` | Web UI |
| GET | `/api/status` | JSON status (power, SWR, band, mode) |
| GET | `/api/config` | Current configuration |
| POST | `/api/config` | Update configuration (restarts device) |
| POST | `/api/command/toggle` | Toggle antenna A/B |
| POST | `/api/command/memory_tune` | Memory tune |
| POST | `/api/command/full_tune` | Full tune |
| POST | `/api/command/bypass` | Toggle bypass |
| POST | `/api/command/auto` | Set auto mode |

### Server-Sent Events (SSE)

Connect to `http://<esp32-ip>/api/events` for real-time meter updates. The browser UI uses this automatically. Remote units also connect here to receive commands.

## MQTT Topics

### Telemetry (published)

| Topic | Description |
|-------|-------------|
| `ldg/tuner/telemetry/fwd_power` | Forward power (Watts), JSON |
| `ldg/tuner/telemetry/ref_power` | Reflected power (Watts), JSON |
| `ldg/tuner/telemetry/swr` | SWR ratio |
| `ldg/tuner/telemetry/band` | Current band |
| `ldg/tuner/status` | Online/offline status (retained) |

### Commands (subscribed)

| Topic | Description |
|-------|-------------|
| `ldg/tuner/command` | Command JSON: `{"command": "full_tune"}` |

Valid commands: `toggle`, `memory_tune`, `full_tune`, `bypass`, `auto`

Response published to `ldg/tuner/command/response`.

## Protocol Reference

### Serial Communication

- **Baud**: 38400
- **Format**: 8N1
- **Levels**: TTL (5V)

### Command Sequence

1. Send wake: two space characters (`0x20`) with 1ms delay
2. Send command byte
3. Wait 250ms minimum before next command
4. Read response (control mode) or meter telemetry (meter mode)

### Commands

| Byte | Function | Response |
|------|----------|----------|
| ` ` (0x20) | Wake (send 2x) | None |
| `A` | Toggle antenna | `A` or `B` |
| `T` | Memory tune | `T` (good), `M` (okay), `F` (fail) |
| `F` | Full tune | `T`, `M`, `F` |
| `P` | Toggle bypass | `P` |
| `C` | Auto mode | `A` |
| `M` | Manual mode | `M` |
| `Z` | Sync | Status byte |
| `S` | Start meter telemetry | None |
| `X` | Stop meter telemetry | None |

### Meter Telemetry

6 bytes + `;;` (0x3B3B) end-of-message marker:

| Bytes | Description | Byte Order |
|-------|-------------|------------|
| 0-1 | Forward power (raw) | Big-endian |
| 2-3 | Reflected power (raw) | Big-endian |
| 4-5 | Band indicator | Big-endian |

### Power Calculation

```
P = ((1000 * V_psu * raw) / (65536 * 0.707))^2 / 50
```

Where `V_psu` is the tuner's power supply voltage (configurable via web UI, default 13.8V).

### Band Detection

| Band | Indicator Range |
|------|----------------|
| 160m | 8230 - 9145 |
| 80m  | 4110 - 4710 |
| 60m  | 3060 - 3080 |
| 40m  | 2250 - 2355 |
| 30m  | 1600 - 1630 |
| 20m  | 1140 - 1180 |
| 17m  | 900 - 915 |
| 15m  | 766 - 782 |
| 12m  | 655 - 662 |
| 10m  | 550 - 590 |
| 6m   | 320 - 360 |

## Remote Unit Link

When the tuner is located near the antenna (e.g., in a shack attic or external enclosure), use a remote unit configuration.

### Architecture

```
[Display Unit] <---WiFi AP---> [Remote Unit] <---TTL---> [LDG Tuner]
  (in shack)       HTTP/SSE       (at antenna)
  - Web UI
  - SSE server                  - SSE client (receives commands)
  - Touchscreen                 - HTTP POST (sends telemetry)
  - HTTP POST receiver          - Executes commands
```

No MQTT broker required for the link. The remote unit connects to the display unit's WiFi AP and communicates over HTTP/SSE.

### How It Works

**Remote Unit** (`esp32-remote`):
- Connects to display unit's WiFi AP
- Opens SSE connection to display unit for commands
- POSTs telemetry to display unit every 200ms
- Uses HTTP Basic Auth (configured via web UI)

**Display Unit** (`esp32-display`):
- Creates WiFi AP for remote unit
- Accepts telemetry POSTs, pushes to SSE clients (browser UI)
- Pushes commands to SSE when web UI/MQTT commands arrive
- Falls back to local tuner when remote disconnects

### MQTT (Optional)

If you have an MQTT broker and want Home Assistant integration, both units can also connect to it. Configure broker credentials via the web UI. The remote unit publishes to `ldg/tuner/remote/telemetry` and the display unit subscribes. This is additive to the HTTP/SSE link, not a replacement.

## Security

### Threat Model

This device will inevitably be exposed to networks by people who shouldn't. Defense in depth:

1. **HTTP Basic Auth** on all web endpoints
2. **Rate limiting** (10 requests/second per IP)
3. **Login lockout** (5 failed attempts = 5 minute ban)
4. **MQTT authentication** (username/password)
5. **No open relays** — device doesn't proxy or forward

### Hardening Recommendations

- **Never** port-forward the HTTP port to the internet
- Use MQTT TLS if your broker supports it
- Change the default web password immediately
- Place on an isolated IoT VLAN if possible

## Build Matrix

| | `esp32-display` | `esp32-remote` |
|---|---|---|
| RAM | ~118KB (36%) | ~48KB (15%) |
| Flash | ~1.1MB (63%) | ~960KB (52%) |
| Touchscreen | Yes | No |
| Web UI | Yes | Yes |
| MQTT | Yes | Yes |
| Remote Link | Server | Optional Client |

`esp32-3248s035c` provides the display variant's features using the onboard
ST7796/GT911, 4 MB flash and internal RAM (no PSRAM required).

## CI/CD

This project uses GitHub Actions for automated builds:
- Every push to `main` runs native touch tests and builds all three firmware variants and their LittleFS images
- Tags (e.g., `v1.0.0`) create a GitHub Release with pre-built binaries
- The `main` branch deploys the web flasher to GitHub Pages

## License

Software: [GPL-3.0](LICENSE)
Hardware designs: [CC BY-SA 4.0](LICENSE-HARDWARE)

## Credits

- Protocol reverse engineering: Efpophis and contributors ([LDGControl](https://github.com/Efpophis/LDGControl))
- Meter power formula: Community-contributed (see LDGControl source)
- Band detection thresholds: Ardugnome (LDGControl, November 2022)
