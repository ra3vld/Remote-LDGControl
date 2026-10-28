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
   and not retained. Before receiving measurements, readings are null/unknown.
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
   configurations, fresh state, and command subscriptions after reconnect.
7. Abruptly power off the controller. After the broker detects the lost connection
   (allow for the 60-second MQTT keepalive), `/status` must become retained `offline`
   and HA entities unavailable. Restart and expect availability/state recovery.
8. Connect a second controller. Expect two devices with different topics and IDs.
   Commands to one board must not execute on the other. Shared legacy topics
   `ldg/tuner/command` and `ldg/tuner/remote/command` must not execute commands.
9. On a display controller, connect a remote with a distinct band, mode, and
   antenna. MQTT and web/SSE must report those remote values and `source: remote`;
   HA commands must reach the remote tuner. Disconnect it and verify local values
   and routing return. Also check the 60-second timeout for a silent remote stream.
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
