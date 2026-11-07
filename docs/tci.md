# TCI: choose the antenna before transmitting

English | [Русский](tci-ru.md)

The ESP32 connected to the LDG tuner can read a TCI server directly over the
LAN. No PC bridge is needed. The implementation uses the common TCI 1.9–2.1
commands, including ExpertSDR3 with SunSDR2 DX. The radio connection is TCI;
an OmniRig driver is not included.

## Setup

1. Enable the TCI server in ExpertSDR3. Allow LAN connections through the PC's
   firewall; note the configured port and the PC's LAN address.
2. On the controller physically connected to the tuner, open **Settings →
   Трансивер · TCI**. Enter the PC's address (not `localhost`, not a `ws://` URL),
   port, and receiver index. `0` is the first software transceiver, `1` the second.
   The selected receiver must match the transmitter you use.
3. Enable TCI and press **Сохранить TCI**. Settings apply without restarting.
   Defaults are disabled, empty host, port `40001`, receiver `0`.
4. Enable the existing band-to-antenna table and assign ANT 1/2. Choose
   “Не переключать” for bands that must not change the antenna.
5. Wait for complete TCI state, fresh zero-power LDG telemetry, and antenna
   initialization. Check the confirmed port before transmitting.

With a separate display and tuner unit, configure TCI on the tuner unit.
The display receives the radio snapshot through its existing status link.
Install both the application and **LittleFS** (`uploadfs`) when deploying this
feature, then reload the browser. No settings erase or partition change is needed.

## Selection rules

- The selected receiver's future TX frequency is VFO A, or VFO B in split,
  plus enabled XIT. `TX_FREQUENCY` in the bootstrap before `READY` is ignored:
  ExpertSDR3 can send the previous transmission's frequency while receiving
  on another band. An updated notification after `READY` takes precedence
  within the derived band. A conflicting band suspends selection: that global
  notification may describe another software receiver. Changes to VFO, split
  or XIT invalidate an older notification. RIT does not change the TX band.
- Initial state is queried without changing radio settings. RX/TX and TUNE
  must be known for every reported software transceiver; any TX or TUNE blocks
  automatic relay writes. Up to 16 software transceivers are accepted. Missing
  required states never default to RX, zero XIT, or split off.
- A known target band must remain stable for 200 ms. Before **each** automatic
  relay toggle, the controller rechecks RX, no TUNE, target band and actual LDG
  zero power no older than 500 ms, including after UART synchronization/wake.
  Missing or positive power waits; starting TX defers selection until RX.
- Confirmed named selections are idempotent. Manual commands cancel pending
  work and hold the choice until a different target band, including across a
  reconnect. The existing UART acknowledgements and mode restoration are used.
  Preventive selection never sends memory/full tune commands without RF.
- Connected but incomplete TCI state suspends preventive selection. Loss of
  Wi-Fi/TCI or a heartbeat older than 3 seconds cancels its pending intent and
  returns to the existing RF-based LDG automation (including its <100 W rule).
  Reconnection clears radio state and requires a new synchronization.
  A live connection with conflicting state stays connected and reports the
  waiting reason; it does not authorize antenna selection. Errors distinguish
  initial synchronization timeout, malformed state, and heartbeat timeout.

TCI is an observer; it never changes frequency, split, XIT, TUNE or PTT.
It cannot guarantee preparation if transmission starts immediately after a
frequency change: there is no PTT interlock in this feature.

The antenna table uses inclusive frequency envelopes in MHz:

| Band | Frequency |
|---|---|
| 160 m | 1.8–2.0 |
| 80 m | 3.5–4.0 |
| 60 m | 5.0–5.5 |
| 40 m | 7.0–7.3 |
| 30 m | 10.1–10.15 |
| 20 m | 14.0–14.35 |
| 17 m | 18.068–18.168 |
| 15 m | 21.0–21.45 |
| 12 m | 24.89–24.99 |
| 10 m | 28.0–29.7 |
| 6 m | 50.0–54.0 |

These envelopes select antenna-table rows; they do not grant TX permission.
Other frequencies are displayed but never select an antenna automatically.

## API and diagnostics

Authenticated `GET /api/radio-config` returns the settings.
Authenticated `POST /api/radio-config` atomically replaces the separate
versioned `radioV1` NVS record; a failed validation/write preserves the previous
configuration. A successful response is `{"result":"ok","restart":false}`.

```json
{"enabled":true,"host":"192.168.1.10","port":40001,"receiver":0}
```

Host: IPv4 address or DNS name, at most 63 ASCII letters/digits/dots/hyphens;
port: integer 1–65535; receiver: integer 0–15. The configured server uses plain
WebSocket at `/`, independently of the controller web UI's optional TLS.

`GET /api/status`, WebSocket snapshots and the display link contain `radio`
with connection state, RX/TX, TUNE, RX and future TX frequencies in Hz, split,
XIT, band, device/protocol, state age and error. `antenna_source` identifies
`tci` or `ldg`; existing LDG measurements retain their meaning.
The logger bridge receives a compact radio snapshot through a separate MQTT
topic; see [MQTT API 2](logger-bridge.md). It does not drive band automation.

`radio.free_heap`, `largest_block` and `stack_free` report bytes of internal
free heap, largest free internal block and TCI-task stack high-water headroom.

[TCI protocol](https://github.com/ExpertSDR3/TCI)
