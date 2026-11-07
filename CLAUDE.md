# Repository guidance

ESP32 tuner firmware built with PlatformIO and Arduino. See [README](README.md)
for supported hardware and [development commands](docs/development.md) for builds
and checks. The default target is `esp32-3248s035c`; `esp32-display` uses the BTT
display and `esp32-remote` is headless.

## Working rules

- Preserve unrelated changes in the shared workspace.
- Keep web endpoints authenticated; WebSocket access uses the short-lived token
  issued by the authenticated `/api/ws-token` endpoint.
- Web HTTP/HTTPS is controlled by `TLS_ENABLED`. MQTT TLS is selected separately
  by broker port 8883; retain certificate and hostname verification.
- Build both application and LittleFS when changing the web interface. Edit
  `data/index.html` and `web/` sources; generated modules belong under `.pio/`.
- Use `scripts/apply_patches.py` through the PlatformIO hooks for managed library
  patches. Do not hand-edit downloaded dependencies as the only copy of a fix.
- Persist user settings through the configuration manager and expose them in
  the settings interface. Preserve migrations and atomic save behavior.
- Keep meter ranges, mode and antenna state consistent across LCD, browser,
  MQTT and the active local/remote tuner.
- UART is 38400 baud, 8N1, 5 V TTL; USB diagnostics are 115200 baud. Preserve
  level conversion, fresh-power guards and acknowledgement-based state.
- Use `scripts/serial_monitor.py` when a serial reader would otherwise hold a
  board in reset through DTR/RTS.
- Choose board profiles and serial ports from the current session. Flashing and
  network reconfiguration follow the user's current authorization.

## Source map

- `src/main.cpp`: task and startup orchestration.
- `config_manager`: configuration storage and captive portal.
- `tuner_protocol`, `tuning_automation`: UART commands, RF state and CONTROLLED.
- `radio_*`: TCI state and preventive antenna selection.
- `web_server`: authenticated HTTP and live WebSocket telemetry.
- `mqtt_handler`: broker connection, discovery and command dispatch.
- `display_ui`: LVGL layout, meter and touch.
- `web/`, `data/index.html`: browser interface and staged modules.
- `test/`, `tests/`: native, rendering, Python and browser suites.
