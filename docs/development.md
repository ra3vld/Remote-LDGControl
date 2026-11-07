# Building and maintaining the firmware

Use Python 3.10+, the pioarduino Core pinned in `requirements-platformio.txt`,
the framework pinned in `platformio.ini`, and Node.js 18+ for web assets.
CI uses Node.js 24. Install the locked host dependencies:

```sh
python -m pip install -r requirements-platformio.txt
pio --version
npm ci --ignore-scripts --no-audit --no-fund
```

The expected Core version is **6.2.0**. Run the commands in the Python
environment used by your IDE or CLI.

## Windows: VS Code

Install [Node.js LTS with npm](https://nodejs.org/en/download) first (CI uses
Node.js 24). After installation, close all VS Code windows and reopen the
project so the extension receives the updated PATH.

Open PowerShell in the project root. Install the pinned Core into the same
environment that the extension uses, rather than a separate system Python:

```powershell
node --version
npm.cmd --version
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" -m pip install --force-reinstall -r .\requirements-platformio.txt
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" --version
npm.cmd ci --ignore-scripts --no-audit --no-fund
```

Confirm that the version is **6.2.0**, then restart VS Code. The extension's
Build and Upload actions use that environment. If a custom Core location is
configured, use its Python and executable paths instead.

### IncompatiblePlatform with 6.2.0-post.1

The pinned Espressif32 platform `55.03.312-1` rejects Core `6.2.0-post.1` even
though its requirement is `>=6.2.0`. The version check treats the converted
`post.1` suffix as a prerelease. This is the upstream
[pioarduino version-check issue](https://github.com/pioarduino/platform-espressif32/issues/563).
Reinstall exact **6.2.0** using the command above. An unrestricted Core upgrade
can select the incompatible post release again. Keep the pinned platform URL
and Arduino framework version in `platformio.ini`.

### WinError 2 in build_web.py

This build hook launches Node.js to generate the browser assets. If Windows
cannot find `node.exe`, install Node.js LTS and reopen VS Code. The hook also
searches the standard Windows installer directories when PATH is stale.
Check `node --version`, then run `npm.cmd ci --ignore-scripts --no-audit --no-fund`
in the project root before retrying Build or Upload. Missing `acorn`, `postcss`
or `terser` means the npm dependencies have not been installed successfully.

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
