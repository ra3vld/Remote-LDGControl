# Separate display and tuner units

When the tuner is located near the antenna (e.g., in a shack attic or external enclosure), use a remote unit configuration.

## Setup

Both controllers join the same reachable Wi-Fi network. In the display unit's
Settings, set **Remote Unit Host** to the headless unit's IP or hostname. Keep
it empty for a directly attached tuner (the default for new configurations).
Both units must use matching web credentials and `TLS_ENABLED` values.

The display polls the remote unit's authenticated `GET /api/status` every
500 ms and sends commands to its `/api/command/*` endpoints. Browsers use
WebSocket telemetry independently. The link needs no MQTT broker or SSE route.
A peer already following another remote source is rejected to prevent loops.
Tuning HTTP requests allow 25 seconds; browser commands allow 30 seconds for
the UART cycle and remote hop. The remote status poll retains its 3-second
timeout. A completed remote command is followed by a status poll.

UART parsing, local commands, remote networking, MQTT, web serving and LVGL
have separate owners. Display buttons enqueue commands without waiting for
UART or DNS. All interfaces select the same active tuner. A queued remote
command fails if its source disappears; it is never redirected to the local
tuner. General command responses are not retried automatically; named automatic
antenna selection has the single guarded recovery attempt described above.

The wire protocol provides antenna toggle only. When the initial antenna is
unknown, an absolute selection may require two acknowledged toggles to reach
the requested port. Repeated requests for a confirmed port do not toggle it.

The mode button cycles MANUAL → AUTO → CONTROLLED → MANUAL. CONTROLLED checks fresh SWR before MEM TUNE and can temporarily enable AUTO
for a persistent mismatch. See [tuning control](tuning-control.md).

## MQTT

Both units can independently connect to a broker and appear as separate Home
Assistant devices. Each uses its own `ldg/tuner/<mac>/...` topics. A display
controller publishes its active tuner; the headless unit publishes its local
tuner. MQTT does not carry the display ↔ remote link; that remains HTTP status polling and command POSTs
(HTTPS with `TLS_ENABLED=1`).
