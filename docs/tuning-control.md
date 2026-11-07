# Controller-managed tuning

The mode button on the LCD and browser cycles **MANUAL → AUTO → CONTROLLED →
MANUAL**. CONTROLLED is a separate application mode on the unit physically
connected to LDG. Temporary physical AUTO during a transmission keeps the
application mode CONTROLLED. Selecting MANUAL or AUTO exits CONTROLLED.

CONTROLLED is disabled by default. The selected CONTROLLED profile and its
options are stored in a separate versioned NVS blob; existing credentials,
antenna tables and calibration retain their layouts. The web settings contain
only fallback options and never select the mode. Saving options does not restart
a transmission or repeat MEM TUNE.

The UART worker establishes acknowledged MANUAL before tuning. Three consecutive
fresh positive-power packets on the same known LDG band confirm a transmission;
unknown-band RF cannot start MEM TUNE. Live TCI TX confirmation can supply the
transmission evidence independently of LDG band decoding. After antenna initialization
and any pending band-table selection, it checks three consecutive fresh SWR
readings. Below the configured threshold it keeps the existing match without
`T` (MEM TUNE), including on subsequent transmissions at an unchanged frequency.
At or above the threshold it issues `T`; unknown/invalid SWR waits for fresh
evidence. This starting SWR gate applies with AUTO fallback either on or off.
The antenna table remains a band-change policy:
manual antenna selection is preserved on subsequent transmissions on that band.
A confirmed automatic change of antenna during an active transmission also arms
one MEM TUNE for the new load.

With band-table selection enabled, before MEM TUNE or SWR fallback the latest
RF band must agree with the confirmed band used by antenna automation, or a
stable TCI band must own the selection.
The absence of a queued antenna command alone does not mean the band debounce
has finished. This prevents tuning the old antenna while the new band is still
being confirmed.

In CONTROLLED, missing post-command telemetry retains an unstarted band-table
selection beyond the initial data-wait timeout. A fresh safe packet resumes the
same operation, so M, antenna selection, MEM TUNE and optional AUTO can complete
within one continuous transmission. Freshness is still limited to 500 ms; held
values never authorize a command. Manual commands, a new band/table or unsafe
power cancel the pending intent. A written antenna command retains its existing
bounded acknowledgement/recovery rules; a data wait does not blindly retry A.

Optional **При высоком КСВ включать AUTO до конца передачи** is off by default.
Its SWR threshold defaults to 2.0 and accepts 1.1–10.0 in steps of 0.1. After MEM
TUNE, three consecutive fresh packets at or above the threshold authorize `C`
(AUTO). Samples from before the memory cycle, invalid SWR, stale/absent telemetry
and zero RF cannot enable AUTO. If MEM TUNE returns a failure but fresh high SWR
confirms the mismatch, the same fallback is available. There is no FULL TUNE `F`
in this policy. SWR supervision continues for the entire transmission, including
after a successful match or a telemetry gap. Three consecutive fresh samples
below the threshold arm one new tuning cycle. Three subsequent consecutive fresh
samples at or above the threshold start MEM TUNE again, even on the same band
and antenna. If the physical tuner is in AUTO or its mode is unknown, acknowledged
`M` precedes `T`. High SWR after that memory cycle can enable `C` again. With the
fallback option off, new SWR rises still repeat MEM TUNE but never enable AUTO;
the same editable SWR threshold applies with either checkbox setting.
Constant high SWR without an intervening confirmed match cannot loop tuning.
Missing or invalid samples break the sample streak and never authorize a command;
waiting for fresh SWR does not disarm supervision.

AUTO stays active between cycles until a new SWR rise starts another memory
cycle or the transmission ends, when `M` restores MANUAL. Mode writes allow one
synchronized retry; memory cycles never loop on failures. Power at or above
100 W blocks tuning/fallback for that transmission before fallback AUTO is active;
a later power reduction alone does not rearm it.

With live TCI state, either TRX or TUNE asserts transmission intent. ExpertSDR3
can report TRX=false and TUNE=true while sending a tuning carrier. Fresh positive
RF packets are still required before MEM TUNE or AUTO; TUNE alone cannot start
a tuning command. Zero-power packets while either flag is on are RF gaps, and a
fresh zero-power packet with both flags off ends the transmission.
Without reliable TCI state, the controller requires actual zero-power packets
spanning at least 1.5 seconds,
with no gap between packets longer than 3 seconds. This accepts idle telemetry
delivered in batches; command decisions still require power no older than 500 ms.
Missing packets do not count as zero or prove the end of transmission. If telemetry is
lost, AUTO restoration waits for fresh evidence. Normal short SSB gaps therefore
do not retrigger MEM TUNE; a longer RF pause without TCI is indistinguishable
from a new transmission.

An explicit command from the display, browser or Home Assistant cancels unstarted
managed work for the current transmission. A started UART operation finishes
before the queued command. The next new transmission reasserts MANUAL and checks
the starting SWR before arming MEM TUNE. Explicit MANUAL/AUTO mode commands exit
CONTROLLED immediately and send
the corresponding LDG command; no deferred AUTO restoration overrides MANUAL.

`GET/POST /api/tuning-config` requires normal web authentication:

```json
{"auto_fallback":true,"swr_threshold":2.0}
```

Both options are required; a `mode` field is rejected. Invalid values return
HTTP 400; failed NVS persistence returns HTTP 500 and preserves the running
settings. Concurrent option saves merge with the current mode under one storage
lock, so a stale form cannot re-enable CONTROLLED.

Authenticated mode commands are `POST /api/command/manual`, `/api/command/auto`
and `/api/command/controlled`; MQTT accepts the corresponding command names and
publishes a Controlled mode button in HA discovery. Mode selection uses the
normal UART command queue, including remote forwarding. Persistence must succeed
before the policy or UART mode changes. CONTROLLED is application command 0x83;
the LDG receives `M`, never 0x83.

`/api/status` and WebSocket telemetry report `mode: 4` for CONTROLLED and the
physical acknowledged mode separately in `tuner_mode`. Existing numeric modes
0–3 retain their meanings. MQTT reports `mode: "controlled"` and e.g.
`tuner_mode: "auto"` during fallback. `tuning_mode` remains a compatible policy
flag and `tuning_state` carries operation progress; remote forwarding preserves
both the application mode and physical state. A lost physical mode ACK is shown
as an error/unknown physical mode, not a confirmed MANUAL.

Validation uses a pure session/SWR policy suite, wire-level UART simulations,
production LVGL rendering and browser-script tests. Hardware timing and actual
RF tuning require a separate deployment and on-air acceptance test.
