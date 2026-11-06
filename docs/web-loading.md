# Web asset build and staged loading

The browser interface has no downloaded JS/CSS libraries, chart framework,
fonts or CDN dependencies. The existing native Canvas drawing is retained.
Terser, PostCSS and Acorn are host build tools only; they never enter LittleFS
or run on the ESP32.

## Loading order

1. The HTML shell includes minified CSS, command handlers, WebSocket setup,
   numeric FWD/REF/SWR, confirmed mode/antenna/band and critical settings.
   Controls become available as soon as this inline core executes. An incomplete
   core leaves the command fieldset disabled.
2. `/api/status` is the first queued read, followed by `/api/config`,
   `/api/tuning-config` and `/api/antenna-config`. The WebSocket token/connection
   starts independently. Critical settings are the shared power scale,
   CONTROLLED fallback/SWR threshold and antenna automation enable flag.
   Values stay disabled until their settings have been confirmed.
3. **Other settings** is an on-demand module: network, MQTT, credentials, tuner
   labels/PSU, display orientation/brightness, antenna band table and TCI.
   It reuses the configuration/antenna snapshots already read; only then does
   it request radio and brightness configuration. Opening settings before
   startup completes waits for the critical reads.
4. The meter and history renderer is an optional module scheduled after the
   critical read sequence, when the browser is idle (with a fallback timer).
   History collection starts with the first accepted telemetry, before this
   module arrives. On mobile, command buttons precede the meter.

Critical Russian strings are inline. The rest of the Russian dictionary loads
separately after startup, or before opening the full settings form. English
works without that module; if it fails, critical Russian controls still work,
with English fallback for optional captions. Switching to Russian for the first
time can therefore finish optional translations asynchronously.

Only one queued JSON read runs at a time, including consuming its body. Status
reads take priority over configuration reads. Commands and settings writes
interrupt and requeue an active background read instead of waiting for it.
Reads have a 5-second deadline; optional scripts have an 8-second deadline and
load one at a time. These browser deadlines do not change device command timing.
A script that arrives after its deadline cannot install over a newer attempt.

Renderer load/Canvas failures leave numeric readings, commands and telemetry
active, with an explicit retry. A failed settings module leaves its fieldset
disabled and offers a retry. Modules register after initialization; they do not
replace or reconnect the core transport. A retry does not erase collected
history. Refreshing/closing the page still discards history.

## Source and build

- `data/index.html`: readable critical HTML shell with build placeholders.
- `web/core.js`, `staging.js`, `boot.js`: inline application core and loading.
- `web/settings.{html,js}`: optional forms and their handlers.
- `web/widgets.{html,js}`: optional Canvas rendering and history controls.
- `web/messages.ru.json`, `style.css`: translations and styles.
- `scripts/build_web.cjs`: safe minification, hash/SRI generation and gzip level 9.

Install Node.js 18 or newer (CI uses 24) and run once:

```sh
npm ci --ignore-scripts --no-audit --no-fund
```

The PlatformIO pre-hook builds assets into each environment's private
`.pio/build/<environment>/web-assets/` and selects that directory for LittleFS.
Use the normal firmware/buildfs targets. Do not directly upload the source
`data/index.html`, which contains placeholders. Files in other `data/`
directories are copied unchanged, including local certificates when present.
Generated assets and manifests belong under ignored `.pio/`, not in Git.

The root handler serves `/` and `/?asset=<kind>.<12 hex digits>.js`, with a
strict filename whitelist and the existing authentication. It selects the
prebuilt `.gz` file when accepted, sets `Content-Encoding`, `Vary` and the stored
`Content-Length`, and streams through the existing 512-byte stack buffer. Plain
files remain available for clients that do not accept gzip. Neither compression
nor a full-file buffer runs on the device. HTML is `no-store`; hashed assets are
private, immutable and cached for one year. Script integrity checks reject
corrupt, partial or mismatched modules.

Install matching **application and LittleFS images together** for this change:
older firmware does not implement the new asset query handler. No device was
flashed during implementation.

## Measurements and limits

Measured with the locked host dependencies, before/after the staged refactor:

| Transfer | Bytes | Change |
| --- | ---: | --- |
| Previous single HTML, plain | 143,369 | baseline |
| Previous single HTML, gzip only | 30,752 | comparison |
| New initial core, plain | 58,804 | −59.0% vs plain baseline |
| New initial core, gzip | 16,014 | −88.8% vs plain; −47.9% vs gzip baseline |
| Core + visuals, English, gzip | 20,030 | other forms not loaded |
| Core + visuals + full Russian, gzip | 24,895 | other forms not loaded |
| All modules, gzip | 30,265 | −78.9% vs plain; −1.6% vs gzip baseline |
| All modules, plain | 103,670 | −27.7% vs plain baseline |

Totals exclude HTTP/TCP headers and dynamic API traffic. Splitting sacrifices
some compression across module boundaries; the main gain is earlier control
readiness and avoiding unopened settings. Transfer-byte savings are not a
measured wall-clock Wi-Fi speedup. Cached modules further reduce subsequent
transfers, while the small HTML core still reloads.

Linked `.dram0.*`, `.iram0.*` and `.rtc.*` section sizes match the pre-refactor
firmware in all three ESP32 builds: no new permanent RAM or file buffers.
Request-local strings and HTTP headers still use existing heap/stack facilities;
this is not a claim of zero transient allocation or a physical heap measurement.
The charts remain entirely browser-side.

## Validation

```sh
npm run build:web
node tests/test_web_assets.cjs
for test in tests/test_*_web.cjs; do node "$test" || exit; done
python3 -m unittest discover -s tests -p 'test_*.py'
pio test -e native -e native-display
pio run -e esp32-display -e esp32-remote -e esp32-3248s035c
pio run -e esp32-display -e esp32-remote -e esp32-3248s035c -t buildfs
```

Real Chromium checks require Playwright available locally (or through
`NODE_PATH`). Set `CHROMIUM_EXECUTABLE` if using a system browser, then run
`tests/test_progressive_browser.cjs`, `test_language_browser.cjs`,
`test_history_browser.cjs` and `test_orientation_browser.cjs` with Node.
They cover held/truncated scripts, SRI, unavailable Canvas, incomplete core,
settings retry, early telemetry/history/commands and 320/375/1280 px layouts.
The VM tests also verify read interruption/requeue and late-script rejection.
The host C++ harness executes the production root handler's gzip/plain,
authentication, cache, content-length and restricted-path branches.
