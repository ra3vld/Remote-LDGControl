# Building and maintaining the firmware

Use Python 3.10+, the pioarduino Core pinned in `requirements-platformio.txt`,
the framework pinned in `platformio.ini`, and Node.js 18+ for web assets.
CI uses Node.js 24. For direct web-tool development and browser tests, install
the host dependencies:

```sh
python -m pip install -r requirements-platformio.txt
pio --version
npm ci --ignore-scripts --no-audit --no-fund
```

The expected Core version is **6.2.0**. Run the commands in the Python
environment used by your IDE or CLI.

## Windows: VS Code

Install the pinned Core into the Python environment used by your IDE. If the
extension uses the standard location, run from the project root:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" -m pip install --force-reinstall -r .\requirements-platformio.txt
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" --version
```

Confirm Core **6.2.0**, then restart VS Code. If a custom Core location is
configured, use its Python and executable paths instead.

### Automatic web-tool setup

Press **Build** or **Upload**. The hook finds Node.js 18+ and npm in PATH or
standard Windows install directories. If either is missing or Node.js is too
old, it downloads portable **Node.js 24.19.0** for Windows x64/ARM64 from
[nodejs.org](https://nodejs.org/download/release/v24.19.0/). The archive is checked
against its pinned SHA-256 before extraction into `.pio/web-tools`.
No administrator rights, system installation or PATH changes are needed.

The hook runs npm's JavaScript entry directly to avoid PowerShell execution
policy and `npm.cmd` lookup issues. It installs the versions from
`package-lock.json` with `--include=dev --ignore-scripts --no-audit --no-fund`.
Dependency setup repeats only when the manifest/lock, runtime or platform
changes, or required modules are missing. Simultaneous Build/Upload tasks share
a setup lock. Failed setup does not create a success stamp and can be retried.

First setup needs internet access. Subsequent builds reuse the local runtime
and `node_modules`. Deleting `.pio/web-tools` clears the setup cache; deleting
`node_modules` causes automatic dependency installation on the next build.
Manual Node/npm version checks and `npm ci` are not part of this workflow.

### IncompatiblePlatform with 6.2.0-post.1

The pinned Espressif32 platform `55.03.312-1` rejects Core `6.2.0-post.1` even
though its requirement is `>=6.2.0`. The version check treats the converted
`post.1` suffix as a prerelease. This is the upstream
[pioarduino version-check issue](https://github.com/pioarduino/platform-espressif32/issues/563).
Reinstall exact **6.2.0** using the command above. An unrestricted Core upgrade
can select the incompatible post release again. Keep the pinned platform URL
and Arduino framework version in `platformio.ini`.

### Web-tool setup errors

An older checkout can report `WinError 2` from `build_web.py` when `node.exe`
is absent. Update the project to use automatic setup. If downloading Node.js or
installing npm dependencies fails, the build prints the failed step. Restore
access to `nodejs.org` / the configured npm registry and retry Build. Existing
system Node/npm installations can also be used without downloading a runtime.

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
