# MQTT / Home Assistant smoke checks

Use an isolated test broker / HA installation for the following checks.

1. Build firmware and LittleFS for the board. Flash **both** images when updating
   the settings UI. Save the broker connection in Settings, keeping auto discovery
   enabled; verify the displayed base is `ldg/tuner/<12 lowercase MAC digits>`.
2. In Home Assistant, connect MQTT to that broker using the default `homeassistant`
   discovery prefix. Expect one device with six sensors and six buttons. Read
   its discovery configs with `mosquitto_sub -h BROKER -t 'homeassistant/+/ldg_MAC/+/config' -v`.
   Replace BROKER and MAC; add the broker's authentication flags if needed.
3. Observe `ldg/tuner/MAC/#`. `/status` is retained `online`; `/state` is JSON
   and retained. Before receiving measurements, readings are null/unknown.
   Stop RF and let telemetry expire: the last observed readings must remain in
   HA with `held: true`. Valid zero-power packets must show zero power while
   keeping the last valid SWR/band. Resume RF and verify fresh values replace
   the held values. Web/HTTP power and SWR still expire after three seconds.
   LCD and browser must keep the same last band with `(last)` and
   `band_held: true`; the next valid band removes that marker. On the LCD,
   digital FWD/REF watts must match the live readings for both 100/1000 W
   scales, show `--` on expiry, and show `0.0W` on actual zero-power frames.
4. With real meter frames arriving, vary power beyond the documented thresholds:
   regular state publications must be at least 1000 ms apart. Hold readings steady
   or vary below thresholds: the latest state must be sent after 30000 ms. Verify
   power returning to zero triggers a fast update, then zero is repeated every
   30 seconds. Connect/discovery/completed-command updates bypass this throttle.
5. Press each HA button with a tuner connected. Exactly one existing command must
   execute, a non-retained `/command/response` must report its result, and state
   must refresh. Do not use retained command messages. Check actual antenna/mode
   and tune results on the tuner.
6. Restart HA, then the broker. Expect the same device/entity IDs, all 12 discovery
   configurations, the retained last-reading state, and command subscriptions
   after reconnect. The `held` attributes must distinguish preserved readings.
7. Abruptly power off the controller. After the broker detects the lost connection
   (allow for the 60-second MQTT keepalive), `/status` must become retained `offline`
   and HA entities unavailable. Restart and expect availability/state recovery.
8. Connect a second controller. Expect two devices with different topics and IDs.
   Commands to one board must not execute on the other. Shared legacy topics
   `ldg/tuner/command` and `ldg/tuner/remote/command` must not execute commands.
9. On a display controller, connect a remote with a distinct band, mode, and
   antenna. MQTT and web/SSE must report those remote values and `source: remote`;
   HA commands must reach the remote tuner. Disconnect it and verify local values
   and routing return. Check the three-second timeout for a silent remote stream;
   old remote readings must not be presented as local measurements.
10. Disable discovery with the broker still configured and reachable. After the
    restart, this board's 12 retained config records must be gone, while normal
    state and command topics work. Re-enable and expect the same IDs to return.
11. Save settings with an empty MQTT password field; authentication must continue
    working. Verify `GET /api/config` never returns either password. Remove the
    broker address and restart: MQTT must stay disabled. Restore broker settings,
    boot without Wi-Fi, then restore Wi-Fi and verify MQTT starts. Initial Wi-Fi
    setup retains its existing captive-portal flow, including the deliberate
    reboot after joining a network to release the web port (80 by default,
    443 with `TLS_ENABLED=1`); MQTT reconnects
    after that reboot. An ordinary Wi-Fi outage after boot needs no reboot.
12. Watch the header indicator, **Settings → MQTT**, `GET /api/status` (`mqtt`),
    and the 115200-baud Serial console. Check: no broker → disabled; Wi-Fi down →
    waiting for Wi-Fi; working broker → connected. Wrong password / unauthorized
    client must display the broker's code (4 / 5); an unreachable TCP endpoint
    must display code -2 without claiming DNS is definitely at fault. Stop the
    broker or interrupt Wi-Fi after connecting, then restore it: show the error,
    automatic reconnect, and the last error marked recovered. Repeated publish
    failures must not flood Serial. Stop HTTP access: the header must say status
    unavailable instead of leaving a stale green indicator. Check narrow/mobile
    layout and that polling never overwrites unsaved MQTT form values. These
    checks require both firmware and LittleFS images from the same version.
