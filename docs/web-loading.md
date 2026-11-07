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

PlatformIO prepares the host dependencies automatically before generating the
assets. On Windows it also downloads a verified portable Node.js if a suitable
Node/npm installation is unavailable. Press Build or Upload; no npm terminal
commands are required. See [automatic setup](development.md#automatic-web-tool-setup)
for caching, network access and supported Windows architectures.

For direct use of the web tools outside PlatformIO (development/tests), install
Node.js 18+ with npm (CI uses 24), then run:

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
`Content-Length`, and streams from LittleFS. Plain
files remain available for clients that do not accept gzip. Neither compression
nor a full-file buffer runs on the device. HTML is `no-store`; hashed assets are
private, immutable and cached for one year. Script integrity checks reject
corrupt, partial or mismatched modules.

Install matching **application and LittleFS images together** and reload the
browser afterward. Firmware and generated modules must use the same asset format.


See [development commands](development.md) for asset and browser checks.
