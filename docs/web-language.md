# Web interface language

Use the language selector in the header to choose **Русский** or **English**.
The first visit uses the browser language (Russian for `ru`, English otherwise).
Your choice is saved in `localStorage` under `ldg.language`, separately for each
browser and device origin. If storage is blocked, switching still works for the
current page. Reloading uses the browser language again.

Labels, help, tooltips, placeholders, live statuses, known MQTT errors, meter
captions and notifications switch immediately once their dictionary is available.
Critical Russian strings are inline; optional captions finish loading asynchronously
on the first Russian selection. Unsaved form values and custom
antenna names are preserved. Protocol mode names such as AUTO, MANUAL and
CONTROLLED retain their standard spelling. Unrecognized diagnostic text is
displayed verbatim.

## ESP32 memory budget

The feature adds **0 bytes of ESP32 application RAM**, below the 200-byte limit.
All language state, translation strings and rendering execute in the browser.
Translations live in `web/messages.ru.json`, generated into the inline core and
an optional language module in LittleFS. The firmware streams these prebuilt
assets through its existing 512-byte stack buffer; there is no language API,
NVS setting or additional language state on the ESP32. This is an architectural
RAM bound, not a measured runtime heap result.

See [asset build and loading](web-loading.md) for `npm ci`, the PlatformIO build
hook, gzip, module caching and application/LittleFS installation requirements.
