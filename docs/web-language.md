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


Language selection and translation run in the browser. The preference is not
stored in controller settings and does not change other browsers.

See [web asset build and loading](web-loading.md) for installation.
