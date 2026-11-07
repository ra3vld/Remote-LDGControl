# Meter and backlight settings

## LCD layout

The LCD uses the G5500 panel's dark blue palette and DejaVu Sans lettering.
The crossed needles show forward power in blue and reflected power in amber.
Large digital readings below the meter place FWD on the left and REF on the
right. SWR appears above the needles, turning amber above 1.5 and red above 2.0.
Missing readings show `--`, separately from measured zero.

The header shows frequency and band when TCI is enabled, or the Wi-Fi network
name and tuner band otherwise. `last` marks a retained tuner band; the TCI
connection state and antenna-selection source remain visible beside the mode.
MEM TUNE, FULL TUNE, ANT 1/2, BYPASS and mode switching remain available.
Long antenna names use an ellipsis. Command feedback temporarily occupies the
antenna-status row; the antenna status returns after feedback expires.
Portrait orientations place the commands below the meter.

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

## Display orientation

The web Settings page provides 0°/90°/180°/270° rotation and horizontal/vertical
mirroring for both physical display builds. 0° is the existing 480×320 landscape.
Mirrors apply in the resulting screen axes after rotation. Press **Apply and
save** in that section: it saves just the orientation, preserves other browser
drafts, and applies in the UI loop without rebooting. Portrait orientations
keep the dial above a two-column button layout. Touch coordinates follow the
same hardware transformation; a held finger must be released after a change.
Headless builds and older firmware disable these controls.

`GET /api/config` exposes `display_orientation` only on display builds. A partial
`POST /api/config` accepts an integer 0–15: bits 0–1 are quarter-turns, bit 2
mirrors horizontally and bit 3 vertically. Invalid values, booleans and writes
to a headless build return HTTP 400. Persistence failure leaves the active
orientation unchanged.

Orientation and touch mapping are saved together with the display configuration.
Older saved configurations migrate when settings are saved. Downgrading to
firmware without the current configuration format requires reconfiguration.

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
current settings intact.

Pin assignment reference: [Sunton BSP, CDS_ADC and TPC_LCD](https://github.com/ardnew/ESP32-3248S035/blob/main/include/ESP323248S035.hpp).


## Rendering and diagnostics

The Sunton ST7796 uses an 80 MHz SPI write clock. If a panel shows corruption,
add `-DSPI_FREQUENCY=40000000` to the Sunton build flags. The BTT ILI9488 uses
20 MHz. Neither display requires PSRAM or a full-screen framebuffer.

Needle movement is smoothed; numeric watts, SWR and antenna safety decisions
use the unsmoothed measurements. Missing telemetry clears live readings.
Range changes immediately remap the needles.

`GET /api/status` includes `lcd` diagnostics: `frame_ms`, `max_frame_ms`,
`frames`, `sent_pixels`, `draw_us`, `flush_us`, `free_heap`, `largest_block`
and `spi_write_hz`. Sunton uses vector drawing; the BTT profile can cache the
static meter face. Rendering diagnostics are separate from RF measurements.
