# Building and maintaining the firmware

Use PlatformIO with the framework pinned in `platformio.ini`, and Node.js 18+
for web assets. CI uses Node.js 24. Install the locked host dependencies:

```sh
npm ci --ignore-scripts --no-audit --no-fund
```

## Build targets

```sh
pio run -e esp32-display -e esp32-remote -e esp32-3248s035c
pio run -e esp32-display -e esp32-remote -e esp32-3248s035c -t buildfs
```

The default target is Sunton `esp32-3248s035c`. Build hooks generate minified
web assets and select the matching LittleFS directory. See
[web loading](web-loading.md) and [Sunton installation](boards/esp32-3248s035c.md).

## Automated checks

```sh
npm run build:web
node tests/test_web_assets.cjs
for test in tests/test_*_web.cjs; do node "$test" || exit; done
python3 -m unittest discover -s tests -p 'test_*.py'
pio test -e native -e native-display
```

Browser suites use Playwright and Chromium. Make Playwright available locally
or through `NODE_PATH`, set `CHROMIUM_EXECUTABLE` for a system Chromium, then run:

```sh
node tests/test_progressive_browser.cjs
node tests/test_language_browser.cjs
node tests/test_history_browser.cjs
node tests/test_orientation_browser.cjs
```

`native-display` renders the production LVGL interface. Set
`LDG_REDESIGN_SNAPSHOTS` to an existing directory to export its example states
as PPM images. Browser screenshot fixtures use generated assets and demonstration
API/WebSocket snapshots; keep real network credentials out of fixtures and images.

GitHub Actions builds the three application/LittleFS variants. Tagged builds
publish release artifacts; the main workflow also prepares the web flasher.
