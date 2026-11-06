# Web history charts

The **Control / Управление** page shows history below the meter and controls:
forward and reflected power in watts, SWR, and three state lanes for the
selected operating mode, confirmed antenna and band. The graphs share a time
axis. Move the pointer over a graph or touch it
to inspect values at that time. Power and SWR use separate, automatic scales.

Choose a 1, 5, 15 or 30 minute window. History collects while the page is open,
including while on the Settings tab. **Clear history** clears only this
page. Reloading or closing it discards the history. Each browser tab has its
own independent history; there is no backfill from before opening the page.

History uses only the existing accepted WebSocket snapshots and HTTP status
fallback. It makes no additional requests and does not change polling or the
telemetry format. An old HTTP reply rejected by the live UI is also excluded
from history. Missing/invalid measurements are gaps, whereas measured zero
power remains zero. Lines do not bridge gaps longer than five seconds, and
state lanes stop after five seconds without an update. Unknown mode/antenna
and band are blank. A held band is labelled “last” and uses an amber lane.
Selections appear after live telemetry confirms them.

All collection, validation, storage and rendering for these charts run in the
browser. No chart samples go into localStorage, IndexedDB, ESP32 RAM, NVS or
device files. Browser memory is bounded to 30 minutes and 12,000 snapshots
(whichever limit is reached first). Drawing runs only while Control is visible;
hidden browser tabs can throttle their own incoming updates and timers.

History collection is in the inline browser core; drawing is in the optional
`web/widgets.js` module. Samples collect before it loads. If drawing cannot load,
numeric readings and controls remain available; retrying drawing preserves history.
The ESP32 serves generated assets through its existing 512-byte file buffer.
See [asset build and loading](web-loading.md) for gzip, build and installation.

Build test assets with `npm run build:web` before validation.
Validation: `node tests/test_history_web.cjs`. Real DOM, Canvas, pointer/touch,
responsive layout, reload and network checks: `tests/test_history_browser.cjs`
with Playwright available through `NODE_PATH` and, if needed,
`CHROMIUM_EXECUTABLE=/usr/bin/chromium`.
