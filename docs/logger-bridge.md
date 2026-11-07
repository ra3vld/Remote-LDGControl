# MQTT API 2 for G5500 Logger Bridge

Use the existing broker configured on this controller. The bridge connects to
that broker directly or through Home Assistant. Existing HA buttons and MQTT
commands without request metadata remain compatible.

The per-controller base is `ldg/tuner/<mac>` (12 lowercase hexadecimal digits).
`/state` adds `api_version:2`, `source_token`, `uptime_ms`, `busy`, `connected`,
`meter_age_ms`, and `antenna_source` (`ldg/tci/omnirig`). A live publication is
required before bridge control; a retained snapshot is historical.

Bridge commands use QoS 0, retain disabled:

```json
{"command":"antenna_1","request_id":"01234567-89ab-4cde-8123-456789abcdef","source_token":"123456789abcdef0123456789abcdef0","expires_uptime_ms":12000}
```

`request_id` is a canonical lowercase UUID. `source_token` comes from the live
state and changes on reboot or local/remote recipient/configuration change.
It identifies the command context, not authentication. The deadline uses the
controller's wrapping 32-bit uptime; starting an operation must be within the
next two seconds. It is checked both on arrival and before worker execution.
Expired/context-mismatched requests do not execute. Once an operation starts,
its existing UART/synchronization deadlines apply.

`/command/response` is not retained and echoes `command`, `request_id` and
`source_token`. `result` is `success`, `failed`, `busy`, `expired`,
`target_changed`, `invalid_request`, or `duplicate_conflict`. Success reflects
the existing command executor, including its UART acknowledgement and meter
restart checks. It is not independent physical relay feedback.

A bounded cache remembers the last 16 request IDs/results across MQTT session
reconfiguration. Repeating an identical request returns its result; reusing an
ID with different contents is rejected. The bridge never retries commands.

## Radio state

`/radio/state` is separate from `/state`, keeping the 1024-byte MQTT packet
limit. With a configured source it publishes the latest snapshot every second;
disabling the source sends a final `disabled` state. Payloads are retained for
historical display. Live freshness is three seconds including `age_ms`.

Fields: `provider` (`tci/omnirig/none`), `state`, `source_token`, `receiver`,
`device`, `protocol`, `rx_frequency_hz`, `tx_frequency_hz`, TX `band`, `rx_tx`,
nullable `tuning`, `split`, `xit`, `xit_offset_hz`, `age_ms`, `stable_ms`,
`modulation`, `error`. Unknown states never imply RX or split/XIT off.
Heap and stack diagnostics remain in the local HTTP interface.

TCI reception and antenna automation stay on the tuner controller. The bridge
only displays radio state and submits explicit user commands. It never feeds
MQTT history back into RF interlocks or band-selection decisions. Remote-unit
radio snapshots retain their original age plus link age.

The implemented radio provider is TCI. `omnirig` is reserved in the contract;
an OmniRig driver is not included.
