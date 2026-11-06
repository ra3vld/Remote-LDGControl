# Web interface language

Use the language selector in the header to choose **Русский** or **English**.
The first visit uses the browser language (Russian for `ru`, English otherwise).
Your choice is saved in `localStorage` under `ldg.language`, separately for each
browser and device origin. If storage is blocked, switching still works for the
current page. Reloading uses the browser language again.

Labels, help, tooltips, placeholders, live statuses, known MQTT errors, meter
captions and notifications switch immediately. Unsaved form values and custom
antenna names are preserved. Protocol mode names such as AUTO, MANUAL and
CONTROLLED retain their standard spelling. Unrecognized diagnostic text is
displayed verbatim.

## ESP32 memory budget

The feature adds **0 bytes of ESP32 application RAM**, below the 200-byte limit.
All language state, translation strings and rendering execute in the browser.
Only `data/index.html` changes in the device image: it lives in LittleFS, and
`h_root()` in `src/web_server.cpp` continues to stream it through its existing
512-byte stack buffer. The firmware adds no variables, language API, NVS setting,
JSON field, heap allocation or larger response buffer. The larger page takes
more flash space and transfer time. This is an architectural RAM bound, not a
claim that free heap measured on a running device stays numerically constant.

Build the web image with `pio run -e esp32-3248s035c -t buildfs` (or the matching
board environment). Updating an existing device requires updating LittleFS;
uploading only the application firmware does not update the web page.
