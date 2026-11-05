# Meter and backlight settings

Settings in the browser apply to the local LCD and all connected browsers.
Upload both the application and LittleFS (`uploadfs`) after installing this feature.

## Power range

**Шкала мощности** accepts one common integer upper limit from **1 to 1000 W**
for FWD and REF and an optional logarithmic mapping. Press **Применить и сохранить**
to apply both settings and retain them across restarts. The main **Save settings**
button also saves scale edits, including a range-only or checkbox-only change.
Live status updates do not overwrite an unsaved draft, including edits made
while a save is pending. Defaults are 1000 W, linear.
Digital watts and numeric SWR do not change with the scale.

Touching the LCD dial or clicking the browser dial/scale still selects the
runtime 100/1000 W quick range. From a custom range, the first tap selects 100 W;
from 100 W it selects 1000 W. Taps preserve the logarithmic setting and do not
write flash. A reboot restores the saved range/mapping.

The continuous logarithmic mapping is `log(1 + 99 * P / max) / log(100)` for
`0 <= P <= max`; zero stays zero. Tick values use the inverse mapping. SWR
curves transform both forward and reflected power with the same function, so
their intersections remain meaningful. Over-range powers stop the needle at
the arc endpoint but remain unchanged in the digital readings.

Authenticated `POST /api/meter-scale` examples:

```json
{"meter_scale":600,"meter_logarithmic":true,"save":true}
{"meter_scale":50}
{"meter_logarithmic":false}
{"toggle":true}
```

`save:true` requires both explicit settings and cannot accompany `toggle`.
Invalid input returns HTTP 400; failed persistence returns HTTP 500 and does
not change the live scale. Responses, `/api/status` and WebSocket events expose
`meter_scale`, `meter_logarithmic` and a coherent 32-bit `meter_scale_revision`.
Browsers reject older revisions and do not overwrite unsaved form edits.
The versioned `meterScaleV1` NVS record is separate from network credentials.

## Backlight

Automatic backlight control is available on **ESP32-3248S035C**, using the
onboard light sensor on GPIO34 and active-high backlight on GPIO27. The BTT
and headless profiles report it as unsupported and reject writes.

**Яркость дисплея** provides:

- Manual brightness, 1–100%; default 100%.
- Automatic control; disabled by default.
- Minimum and maximum automatic brightness, 1–100%; defaults 15% and 100%.
- Current applied brightness and raw light-sensor reading (not calibrated lux).
- Calibration for darkness and bright light; either ADC direction is supported.

For calibration, cover the light sensor, wait for the live reading to settle,
then press **Запомнить темноту**. Illuminate the sensor, wait and press
**Запомнить яркий свет**. The readings must differ by at least 100 ADC counts.
Press **Применить и сохранить**. The display stays on while dimming; no automatic
power-off or sleep mode is introduced.

The controller reads four ADC samples every 100 ms. An exponential filter,
2% deadband and changes of at most 2 percentage points per sample suppress
flicker. New limits and manual mode apply directly. There are no sleeps or
flash writes in the sampling loop. PWM is 20 kHz, 8-bit. If PWM setup fails,
the firmware leaves full brightness enabled and reports `active:false`.

Authenticated `GET /api/display-config` returns settings plus `supported`,
`active`, `light_raw` and `brightness`. The live fields also appear under
`display` in `/api/status`. A complete settings replacement uses:

```json
{"automatic":true,"manual":100,"minimum":15,"maximum":100,"dark_adc":3500,"bright_adc":300}
```

`POST /api/display-config` persists `backlightV1` and replies `{"result":"ok"}`.
Invalid values return HTTP 400; a failed save returns HTTP 500 and leaves the
current settings intact. Existing `configV1` and antenna settings are unchanged.

Pin assignment reference: [Sunton BSP, CDS_ADC and TPC_LCD](https://github.com/ardnew/ESP32-3248S035/blob/main/include/ESP323248S035.hpp).

## Verification

### Needle renderer and smoothing

The LCD keeps a lossless, row-indexed compressed RGB565 copy of the static
scales and SWR curves (about 14 KiB for the default face, bounded to 48 KiB).
It is rebuilt when the range, mapping or widget geometry changes. Allocation
failure falls back to vector rendering. An additional 9600-byte scratch buffer
allows comparing old/new dial pixels and sending short changed rectangles over
SPI. This is not a full-screen framebuffer and does not require PSRAM.

The onboard Sunton ST7796 uses an 80 MHz SPI write clock (previously 40 MHz),
matching the [ESP32-3248S035C BSP](https://github.com/ardnew/ESP32-3248S035/blob/main/include/ESP323248S035.hpp).
This halves pixel wire time; CPU rendering and address-window overhead still
contribute to frame time. Stability at 80 MHz needs verification on the actual
panel. If there is display corruption, append `-DSPI_FREQUENCY=40000000` to the
`esp32-3248s035c` environment's `build_flags` and rebuild. The external BTT
ILI9488 stays at 20 MHz; display reads stay at 16 MHz and SPI touch at 2.5 MHz.

Needle strokes use an antialiased narrow rasterizer. Angular motion follows the
latest sample with time-based exponential smoothing: 40 ms rise and 65 ms fall
time constants, approximately 95% response in 120/195 ms. Repeated identical
data does not restart motion. After a stall over 250 ms it resumes at the latest
target rather than replaying missed positions. Missing telemetry resets the
needle; scale changes immediately remap it. Numeric watts, SWR, MQTT/UART data,
and antenna-switching safety checks remain unsmoothed.

`/api/status.lcd` exposes actual measurements:

- `frame_ms`, `max_frame_ms`, `frames`: LVGL refresh duration/count; the maximum
  includes initial face construction and full redraws.
- `pixels`: pixels LVGL requested; `sent_pixels`: pixels actually sent over SPI.
- `draw_us`: current and comparison-image rendering; `flush_us`: flush processing
  including comparison and SPI, so these two times overlap and must not be added.
- `face_bytes`, `face_build_us`, `face_builds`, `cache_hits`, `vector_draws`:
  cache use and fallback diagnostics. Face construction is outside steady-state
  needle performance and occurs only when the static face changes.
- `delta_flushes`: flushes using pixel comparison; `free_heap`, `largest_block`:
  runtime memory available on ESP32.
- `spi_write_hz`: configured display write frequency on ESP32 (not a measured
  clock signal).

HTTP/WebSocket work runs at priority 1 on core 0 alongside the other network
tasks. The UART task retains priority 2 on core 1, where the Arduino UI loop
also runs. Per-request HTTP INFO logging is disabled (`HTTPS_LOGLEVEL=2`);
warnings and errors remain enabled. This prevents network/Serial bursts from
preempting LCD rendering at the former web-task priority 5.

The native benchmark compares legacy vector strokes with the cached/delta path
using identical samples. Its in-memory SPI mock is not a hardware timing model.

On 2026-11-05, the actual ESP32-3248S035C was measured over `/api/status` on
40 m at approximately 40 W, with a 100 W logarithmic scale and 80 MHz SPI.
Each row below summarizes distinct frame counters observed during transmission,
not every rendered frame. The two transmissions had different reflected-power
fluctuations; this is a field comparison, not an identical-input benchmark.

| Web task / HTTP logging | Observed RF frames | Median frame | Observed range |
| --- | ---: | ---: | ---: |
| Core 1, priority 5 / INFO | 55 | 330 ms | 21–920 ms |
| Core 0, priority 1 / warnings/errors | 29 | 55 ms | 27–78 ms |

The second run produced about 12 refreshes/s by frame-counter differences.
All 59 status polls succeeded, including 29 RF samples followed by transmission
ending and idle operation. The largest free heap block stayed at 38,900 bytes;
no vector fallback occurred. The boot/full-redraw maximum was 283 ms and is
separate from the RF sample range. These samples show a 6x median improvement
over the first instrumented candidate; they do not establish a universal 10x
speedup over the original firmware, which had no frame timing diagnostics.

Native tests cover scale math/inversion, state ordering, persistence failures,
both light-sensor directions, smoothing and brightness limits. LCD tests compare
partial and full renders pixel-for-pixel for custom linear/log ranges and burst
telemetry; browser tests cover shared updates, unsaved edits, rejected saves,
calibration and duplicate submissions. Visual smoothness/artifact acceptance,
sensor polarity and usable low-brightness levels remain user checks on the board.
