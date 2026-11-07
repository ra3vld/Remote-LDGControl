# LDG serial protocol

## Serial Communication

- **Baud**: 38400
- **Format**: 8N1
- **Levels**: TTL (5V)

## Command Sequence

1. Stop telemetry with `X` (preceded by wake), allow 250 ms to settle, and drain the final meter bytes.
2. Send wake: two space characters (`0x20`) with 1 ms delay, then the command byte.
3. Read the command-specific acknowledgement. Memory/full tuning waits up to
   20 seconds because a full cycle can take 15 seconds and memory tuning can fall back to it.
4. Wait 250 ms **after receiving the acknowledgement**, then wake and restore telemetry with `S`.

AUTO/MANUAL/BYPASS, antenna and synchronization replies have a 2200 ms deadline, matching the response window
in [NeoLDG](https://github.com/RiskAndReward1337/NeoLDG/blob/main/src/LdgTunerController.cpp).
`Z` must match the complete synchronization string below. Unknown bytes do not
acknowledge a command or establish a connection. A failed tune (`F`) is a valid
reply but reports an unsuccessful operation. After a missing response, a full
`Z` exchange must succeed before another control command is sent, so a late
reply cannot be mistaken for the next operation's result. Commands are not retried.
After a timeout, keep the response parser active for a further 250 ms before
restoring telemetry. A matching reply during this bounded quarantine is exposed
as `uart.last_command.late_ack`, without changing the failed command result;
the post-ACK guard still applies. A late antenna ACK can confirm the selected
port. A failed recovery `Z` that prevented `A` from being sent does not invalidate
an otherwise confirmed antenna.

When an actually sent `A` has no reply, the current `antenna` remains unknown.
The new `antenna_last` field retains the last confirmed port (numeric in web
snapshots, `A`/`B` in MQTT); `antenna_confirmed` distinguishes it from current
confirmation. Web/LCD show the remembered port in amber and label it unconfirmed.
It is never used as evidence to skip a necessary antenna command.

## Startup antenna check and band selection

The tuner and controller are assumed to power up together. On each controller
boot, the UART worker waits for a valid meter frame, no older than 500 ms, with
zero forward raw power and zero calculated forward watts. Missing/expired data
does not mean zero power. It then performs two acknowledged antenna toggles and
verifies that the second reply restores the original port. The normal ACK guard
applies between commands. This check runs even when band selection is disabled;
RF silence later in the session does not repeat it. The web UI shows `Готово`
and the LCD shows `ANT: Ready` only after successful verification.

If either acknowledgement is missing, no blind retry is made. The initialization
stays in error until a confirmed manual antenna selection; automatic band
selection is inhibited until initialization or that manual selection succeeds.
Operators must wait for initialization before transmitting: this firmware does
not control the transceiver's PTT.

Settings → **Антенна по диапазону** enables automatic band selection and assigns
ANT1, ANT2 or **Не переключать** to each band. Use its separate save button.
The default is disabled, with all bands unassigned. The table is saved
independently of network settings. Saving it does not restart or select an antenna.

The authenticated endpoint accepts/replies with
`{"enabled":true,"bands":[0,0,0,1,0,2,0,0,0,0,0]}`. Values are 0 (leave alone),
1 (ANT1), or 2 (ANT2), ordered 160m, 80m, 60m, 40m, 30m, 20m, 17m, 15m, 12m,
10m, 6m. POST requires the complete valid table and returns `result: "ok"` only
after persistence succeeds. Configure the device physically connected to the
tuner; a display proxy does not forward its local table to another controller.

Three consecutive valid RF frames confirm a band. An invalid frame, unknown
band, RF silence, or a gap over 500 ms breaks confirmation. The first confirmed
band also selects its assigned antenna, with the same power and manual-command
guards as subsequent changes. If startup checking has not finished, this choice
waits for successful checking; startup toggles still require fresh actual zero
power. A confirmed manual antenna choice before the first band is respected.
Each band gets one selection operation, including at most one synchronized recovery attempt,
and is consumed even when skipped: any sample at
100 W or above during confirmation, invalid power, an unassigned band, or a
failed antenna command prevents another selection later on that band. Busy
MANUAL negotiation waits for fresh safe telemetry before resuming. Power is
checked again immediately before starting the selection. Only actual local
UART frames drive decisions; retained MQTT data and held band labels do not.

Manual screen/web/HA commands cancel unstarted automatic selection; a started
operation finishes before queued manual commands run. A manual port choice is
never restored from the table while remaining on the same band, and never
rewrites the table. `/api/status`, WebSocket and MQTT snapshots expose
`antenna_init` and `antenna_auto`, including power skips and command errors.

## Commands

| Byte | Function | Response |
|------|----------|----------|
| ` ` (0x20) | Wake (send 2x) | None |
| `A` | Toggle antenna | ASCII `1` or `2` (application/UI names remain `A`/`B`) |
| `T` | Memory tune | `T` (good), `M` (okay), `F` (fail) |
| `F` | Full tune | `T`, `M`, `F` |
| `P` | Bypass (passthrough) | `P` |
| `C` | Auto mode | `A` |
| `M` | Manual mode | `M` |
| `Z` | Sync receive stream | `000000000000000AzAz` (15 ASCII zeroes followed by `AzAz`) |
| `S` | Start meter telemetry | None |
| `X` | Stop meter telemetry | None |

`Z` does not return operating mode or antenna status. These values are known
only from acknowledged control commands; changes made at the tuner's front
panel cannot be discovered from the power telemetry. `P` confirms bypass;
the protocol specification does not promise a toggle or report an exit state.
The LCD and browser button shows **BYPASS OFF** while bypass is confirmed and
**BYPASS ON** otherwise. Pressing **BYPASS OFF** selects AUTO (`C`, acknowledged
with `A`), the bypass exit described in the AT-1000ProII Operations Manual
(Rev A, page 16); it does not send `P` again. Labels change only on state updates,
so an unconfirmed command never displays a successful local toggle.

## Meter Telemetry

6 bytes + `;;` (0x3B3B) end-of-message marker:

| Bytes | Description | Byte Order |
|-------|-------------|------------|
| 0-1 | Forward power (raw) | Big-endian |
| 2-3 | Reflected power (raw) | Big-endian |
| 4-5 | Band indicator | Big-endian |

## Power Calculation

```
P = ((1000 * V_psu * raw) / (65536 * 0.707))^2 / 50
```

Where `V_psu` is the tuner's power supply voltage (configurable via web UI, default 13.8V).

SWR uses the raw amplitude ratio: `(fwd_raw + ref_raw) / (fwd_raw - ref_raw)`.
With no forward power, or reflected power greater than or equal to forward
power, SWR is unknown (`null` in JSON, `--` on screen). Power readings remain
available. Such readings must not be displayed as a good 1:1 match.

## Band Detection

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
