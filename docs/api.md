# HTTP and WebSocket API

The standard build listens on HTTP port 80. `TLS_ENABLED=1` selects HTTPS
port 443. All application endpoints require the configured HTTP Basic Auth.
JSON writes use `Content-Type: application/json`.

## Endpoints

| Method | Endpoint | Purpose |
|---|---|---|
| GET | `/` | Browser interface; generated modules use `/?asset=<filename>` |
| GET | `/api/status` | Active tuner, measurements, radio, command and MQTT diagnostics |
| GET, POST | `/api/config` | Network, MQTT, credentials, labels, PSU voltage, remote host and orientation |
| GET, POST | `/api/antenna-config` | Persistent band-to-antenna table |
| GET, POST | `/api/radio-config` | Direct TCI configuration |
| GET, POST | `/api/tuning-config` | CONTROLLED SWR threshold and AUTO fallback |
| POST | `/api/meter-scale` | Shared scale, logarithmic mapping and optional persistence |
| GET, POST | `/api/display-config` | Sunton backlight configuration and live light reading |
| POST | `/api/mqtt/reconnect` | Request MQTT reconnect without reboot |
| GET | `/api/ws-token` | Obtain the browser's short-lived WebSocket access token |
| POST | `/api/command/toggle` | Toggle antenna |
| POST | `/api/command/antenna_1`, `/api/command/antenna_2` | Select the named antenna |
| POST | `/api/command/memory_tune` | MEM TUNE |
| POST | `/api/command/full_tune` | FULL TUNE |
| POST | `/api/command/bypass` | Enter bypass |
| POST | `/api/command/auto` | AUTO mode; also exits bypass |
| POST | `/api/command/manual` | MANUAL mode |
| POST | `/api/command/controlled` | CONTROLLED application mode |

A command response reports the executor's result, including UART acknowledgement.
An accepted browser click alone is not confirmation. Commands act on the same
active local/remote tuner as the displayed status and are not retried automatically.

## Configuration saves

`POST /api/config` accepts partial updates. Blank password fields preserve stored
passwords. A successful save reports whether a restart is required; MQTT, labels,
PSU voltage, credentials and remote host apply at runtime, while Wi-Fi/static-IP
changes require a restart. Failed persistence leaves the active configuration
unchanged. `{"reset":true}` resets configuration and restarts into setup.

MQTT usernames accept up to 64 bytes and passwords up to 128 bytes. Oversized
values are rejected. Firmware reads older configuration formats and migrates
on save; downgrading to firmware without the current format requires reconfiguration.

Specialized settings use their own save actions. See
[display settings](display-settings.md), [TCI](tci.md),
[CONTROLLED tuning](tuning-control.md) and
[the antenna table](tuner-protocol.md#startup-antenna-check-and-band-selection)
for payloads and allowed values.

## Live telemetry

The browser obtains `/api/ws-token`, then connects to `/api/ws?t=<token>`.
The controller permits two simultaneous WebSocket clients. Other HTTP requests
share the web server's available connections. Close unused browser tabs if all
live connections are occupied.

Snapshots include FWD/REF watts, SWR, mode, antenna, band, meter scale/revision,
command feedback and radio state. Numeric modes are 0 unknown, 1 AUTO,
2 MANUAL, 3 BYPASS and 4 CONTROLLED; `tuner_mode` separately reports the physical
acknowledged mode. Missing SWR is JSON `null`. `band_held` marks the last known
band. `antenna_last` and `antenna_confirmed` distinguish a remembered port from
current acknowledgement.

The page falls back to `/api/status` when live meter updates stop. Older HTTP
responses cannot overwrite newer WebSocket state. Telemetry expires after
three seconds; retained labels never authorize RF-sensitive operations.

## Diagnostics

`/api/status` includes `firmware_build`, `uptime_ms`, `reset_reason`, `mqtt`,
`uart`, and, on display builds, `lcd` and `display`.

- `mqtt`: broker, state, connection attempts, retry delay and last transport error.
- `uart.last_command`: command/phase, received reply, timeout and late ACK status.
- `lcd`: frame/flush timing, transmitted pixels, rendering mode and available heap.
- `display`: backlight support, current brightness and raw ambient-light ADC.
- `radio`: TCI provider, connection state, future TX frequency, RX/TX/TUNE,
  data age and selection source.

The USB console runs at 115200 baud and records connection transitions and
command failures without printing saved passwords.
