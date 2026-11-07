let language = 'en';

try {
            const saved = localStorage.getItem('ldg.language');
            language = saved === 'ru' || saved === 'en' ? saved
                : /^ru\b/i.test(navigator.language) ? 'ru' : 'en';
        } catch (_) {
            if (typeof navigator !== 'undefined' && /^ru\b/i.test(navigator.language)) language = 'ru';
        }

const languageBindings = new Map();

function t(source) {
            return language === 'ru' && Object.hasOwn(languageMessages, source) ? languageMessages[source] : source;
        }

function setText(target, text, property = 'textContent') {
    const element = typeof target === 'string' ? document.getElementById(target) : target;
    if (!element) return;
    if (!languageBindings.has(element)) languageBindings.set(element, new Map());
    const render = typeof text === 'function' ? text : () => t(text);
    languageBindings.get(element).set(property, render);
    element[property] = render();
}

function bindLanguageText(root = document) {
    root.querySelectorAll('[data-i18n]').forEach(element => {
        if (!languageBindings.get(element)?.has('textContent')) setText(element, element.textContent);
    });
    for (const property of ['title', 'placeholder', 'ariaLabel']) {
        root.querySelectorAll('[data-i18n-' + property + ']').forEach(element => {
            if (!languageBindings.get(element)?.has(property)) setText(element, element[property], property);
        });
    }
}

function setLanguage(value) {
    language = value === 'ru' ? 'ru' : 'en';
    try { localStorage.setItem('ldg.language', language); } catch (_) {}
    refreshLanguage();
    if (coreReady && language === 'ru') ensureLocale().catch(() => {});
}

function setConnection(message, dot = '') {
            document.getElementById('connectionDot').className = 'status-dot ' + dot;
            setText('connectionText', message);
        }

document.querySelectorAll('.nav-tab').forEach(tab => {
            tab.addEventListener('click', () => {
                document.querySelectorAll('.nav-tab').forEach(t => t.classList.remove('active'));
                document.querySelectorAll('.tab-content').forEach(c => c.classList.remove('active'));
                tab.classList.add('active');
                document.getElementById('tab-' + tab.dataset.tab).classList.add('active');
                historyVisible = tab.dataset.tab === 'control';
                historyCursor = null;
                scheduleHistoryDraw();
            });
        });

let toastTimer = null;

function showToast(message, type = 'success', pending = false) {
            const toast = document.getElementById('toast');
            setText(toast, message);
            toast.className = 'toast ' + type + ' show';
            clearTimeout(toastTimer);
            if (!pending) toastTimer = setTimeout(() => toast.classList.remove('show'), 3000);
        }

const meterState = {
            fwdPower: null,
            refPower: null,
            swr: null,
            maxPower: 1000,
            logarithmic: false,
            band: '--'
        };

let meterScaleRevision = null;

let meterScaleEpoch = 0;

let scaleRequestPending = false;

let meterSettingsDirty = false;

let meterSettingsLoaded = false;

let meterSettingsRendered = null;

function powerFraction(power) {
            const f = Number.isFinite(power) && power > 0 ? Math.min(power / meterState.maxPower, 1) : 0;
            return meterState.logarithmic ? Math.log1p(99 * f) / Math.log(100) : f;
        }

function powerAtFraction(f) {
            return meterState.maxPower * (meterState.logarithmic ? Math.expm1(f * Math.log(100)) / 99 : f);
        }

function powerTick(value) {
            return value > 0 && value < 1 ? value.toFixed(2) : value > 0 && value < 10 ? value.toFixed(1) : value.toFixed(0);
        }

function meterSettingsEdited() {
            return meterSettingsDirty || (meterSettingsRendered !== null &&
                (document.getElementById('meterRange').value !== meterSettingsRendered.watts ||
                 document.getElementById('meterLogarithmic').checked !== meterSettingsRendered.logarithmic));
        }

function renderMeterSettings(force = false) {
            // Compare with the last rendered form, not the incoming live state.
            // This also protects browser/autofill edits before an event arrives.
            if (!force && (meterSettingsEdited() || scaleRequestPending)) return;
            document.getElementById('meterRange').value = String(meterState.maxPower);
            document.getElementById('meterLogarithmic').checked = meterState.logarithmic;
            meterSettingsRendered = { watts: String(meterState.maxPower), logarithmic: meterState.logarithmic };
        }

for (const id of ['meterRange', 'meterLogarithmic']) {
            const markEdited = () => {
                meterSettingsDirty = true;
                setText('meterSettingsStatus', "Changes pending — click “Apply and save” or “Save settings”.");
            };
            document.getElementById(id).addEventListener('input', markEdited);
            document.getElementById(id).addEventListener('change', markEdited);
        }

function applyMeterScale(data, epoch = meterScaleEpoch) {
            const watts = data.meter_scale;
            const revision = data.meter_scale_revision;
            const logarithmic = data.meter_logarithmic === undefined ? false : data.meter_logarithmic;
            if (epoch !== meterScaleEpoch || !Number.isInteger(watts) || watts < 1 || watts > 1000 || typeof logarithmic !== 'boolean'
                || !Number.isInteger(revision) || revision < 0 || revision > 0xffffffff) return false;
            // HTTP responses can arrive after a newer WebSocket update.
            if (meterScaleRevision !== null) {
                const delta = (revision - meterScaleRevision) >>> 0;
                if (delta === 0 || delta >= 0x80000000) return false;
            }
            meterScaleRevision = revision;
            meterState.maxPower = watts;
            meterState.logarithmic = logarithmic;
            document.getElementById('scaleValue').textContent = watts + 'W' + (logarithmic ? ' LOG' : '');
            meterSettingsLoaded = true;
            document.getElementById('meterRange').disabled = false;
            document.getElementById('meterLogarithmic').disabled = false;
            document.getElementById('btnSaveMeter').disabled = scaleRequestPending;
            if (!meterSettingsEdited()) setText('meterSettingsStatus', "LCD and browsers use the same scale.");
            renderMeterSettings();
            return true;
        }

async function saveMeterSettings() {
            if (!meterSettingsLoaded || scaleRequestPending) return false;
            const watts = Number(document.getElementById('meterRange').value);
            const logarithmic = document.getElementById('meterLogarithmic').checked;
            if (!Number.isInteger(watts) || watts < 1 || watts > 1000) {
                showToast("Power limit must be an integer from 1 to 1000 W", 'error');
                return false;
            }
            scaleRequestPending = true;
            const epoch = meterScaleEpoch;
            const button = document.getElementById('btnSaveMeter');
            button.disabled = true;
            const controller = new AbortController();
            const timeout = setTimeout(() => controller.abort(), 5000);
            try {
                const resp = await fetch('/api/meter-scale', { method: 'POST',
                    headers: { 'Content-Type': 'application/json' }, signal: controller.signal,
                    body: JSON.stringify({ meter_scale: watts,
                        meter_logarithmic: logarithmic, save: true }) });
                if (!resp.ok) throw new Error('HTTP ' + resp.status);
                const data = await resp.json();
                if (epoch !== meterScaleEpoch) throw new Error("Connection changed; check board settings");
                if (data.meter_scale !== watts || data.meter_logarithmic !== logarithmic ||
                    !Number.isInteger(data.meter_scale_revision) || data.meter_scale_revision < 0 || data.meter_scale_revision > 0xffffffff)
                    throw new Error("The board did not confirm the selected scale");
                applyMeterScale(data, epoch);
                // Keep a draft made while this request was in flight.
                if (document.getElementById('meterRange').value === String(watts) &&
                    document.getElementById('meterLogarithmic').checked === logarithmic) {
                    meterSettingsDirty = false;
                    renderMeterSettings(true);
                    setText('meterSettingsStatus', "Scale settings saved.");
                }
                drawMeter();
                showToast("Scale settings saved");
                return true;
            } catch (err) {
                showToast(() => t("Cannot save scale: ") + t(err.message), 'error');
                return false;
            } finally {
                clearTimeout(timeout);
                scaleRequestPending = false;
                button.disabled = !meterSettingsLoaded;
            }
        }

let brightnessLoaded = false;

let brightnessPending = false;

let lightRaw = null;

let lightReadAt = 0;

function updateBacklightStatus(data) {
            if (!data || typeof data.supported !== 'boolean') return;
            lightRaw = Number.isInteger(data.light_raw) && data.light_raw >= 0 && data.light_raw <= 4095 ? data.light_raw : null;
            lightReadAt = Date.now();
            setText('brightnessStatus', () => !data.supported
                ? t("Built-in brightness control is unavailable on this board.")
                : data.active ? t("Brightness: ") + data.brightness + t("% · Sensor: ") + lightRaw
                : t("Waiting for backlight; if this persists, check PWM startup."));
        }

async function toggleMeterScale() {
            if (scaleRequestPending) return;
            scaleRequestPending = true;
            const epoch = meterScaleEpoch;
            const controller = new AbortController();
            const timeout = setTimeout(() => controller.abort(), 5000);
            try {
                const resp = await fetch('/api/meter-scale', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ toggle: true }),
                    signal: controller.signal
                });
                if (!resp.ok) throw new Error('HTTP ' + resp.status);
                const data = await resp.json();
                if (applyMeterScale(data, epoch)) drawMeter();
            } catch (err) {
                showToast(() => t('Cannot change meter scale: ') + t(err.message), 'error');
            } finally {
                clearTimeout(timeout);
                scaleRequestPending = false;
                document.getElementById('btnSaveMeter').disabled = !meterSettingsLoaded;
            }
        }

document.getElementById('scaleValue').addEventListener('click', toggleMeterScale);

let activeAntenna = 0;

let activeMode = 0;

let tunerMode = 0;

function toggleTuningMode() {
            return sendCommand(activeMode === 2 ? 'auto' : activeMode === 1 ? 'controlled' : 'manual');
        }

function toggleBypass() {
            return sendCommand(tunerMode === 3 ? (activeMode === 4 ? 'controlled' : 'auto') : 'bypass');
        }

function selectAntenna(ant) {
            if (ant !== 1 && ant !== 2) return;
            if (commandPending) return;
            if (ant === activeAntenna) { showToast(() => 'ANT ' + ant + t(' already selected')); return; }
            return sendCommand('antenna_' + ant);
        }

function updateAntennaButtons(antenna, last = 0) {
            activeAntenna = antenna;
            const b1 = document.getElementById('btnAnt1');
            const b2 = document.getElementById('btnAnt2');
            b1.className = 'btn ant-btn ' + (antenna === 1 ? 'active' : antenna === 0 && last === 1 ? 'unconfirmed' : 'inactive');
            b2.className = 'btn ant-btn ' + (antenna === 2 ? 'active' : antenna === 0 && last === 2 ? 'unconfirmed' : 'inactive');
        }

function updateOperatingMode(data) {
            if (data.mode !== undefined) {
                activeMode = Number.isInteger(data.mode) && data.mode >= 0 && data.mode <= 4 ? data.mode : 0;
                tunerMode = Number.isInteger(data.tuner_mode) && data.tuner_mode >= 0 && data.tuner_mode <= 3
                    ? data.tuner_mode : activeMode === 4 ? 0 : activeMode;
                setText('btnMode', () => activeMode === 2 ? t('Set Auto') : activeMode === 1 ? t('Set Controlled') : t('Set Manual'));
                setText('btnBypass', () => tunerMode === 3 ? t('BYPASS OFF') : t('BYPASS ON'));
                const modes = { 0: '--', 1: 'AUTO', 2: 'MANUAL', 3: 'BYPASS', 4: 'CONTROLLED' };
                document.getElementById('modeValue').textContent = modes[data.mode] || '--';
            }
        }

function updateMeter(data) {
            applyMeterScale(data);
            updateAntennaStatus(data);
            if (data.radio) updateRadioStatus(data.radio, data.antenna_source);
            meterState.fwdPower = Number.isFinite(data.fwd_power) && data.fwd_power >= 0 ? data.fwd_power : null;
            meterState.refPower = Number.isFinite(data.ref_power) && data.ref_power >= 0 ? data.ref_power : null;
            meterState.swr = Number.isFinite(data.swr) && data.swr >= 1 ? data.swr : null;

            if (data.band) {
                meterState.band = data.band;
                setText('bandValue', () => t('Band: ') + t(data.band) + (data.band_held ? t(' (last)') : ''));
            }
            updateOperatingMode(data);
            if (data.antenna !== undefined) {
                updateAntennaButtons(data.antenna, data.antenna_last);
                if (data.antenna === 0 && (data.antenna_last === 1 || data.antenna_last === 2))
                    setText('antennaInitStatus', () => t("Unconfirmed; last ANT") + data.antenna_last);
            }

            setText('liveFwd', () => 'FWD: ' + (meterState.fwdPower === null ? '--' : meterState.fwdPower.toFixed(1) + t(' W')));
            setText('liveRef', () => 'REF: ' + (meterState.refPower === null ? '--' : meterState.refPower.toFixed(1) + t(' W')));
            setText('liveSwr', () => t('SWR') + ': ' + (meterState.swr === null ? '--' : meterState.swr.toFixed(2) + ':1'));
            drawMeter();
            appendHistory(data);
        }

const historySamples = [];

const historyRetention = 30 * 60 * 1000;

// At most eight real snapshots per 250 ms bucket: first/last and the
// extrema of all three measurements. Capacity covers the full 30 minutes
// regardless of transport rate, including both partial boundary buckets.
const historyBucketMs = 250;
const historyLimit = (historyRetention / historyBucketMs + 2) * 8;

const historyGap = 5000;

const historyModes = ['', 'AUTO', 'MANUAL', 'BYPASS', 'CONTROLLED'];

let historyWindow = 300000;

let historyVisible = true;

let historyCursor = null;

let historyTxOnly = false, historyElapsed = 0, historySegment = 0, historyPrevious = null;

function pruneHistory(now) {
            let count = 0;
            while (count < historySamples.length && historySamples[count].at < now - historyRetention) count++;
            count = Math.max(count, historySamples.length - historyLimit);
            if (count > 0) historySamples.splice(0, count);
        }

function appendHistory(data) {
            if (!['fwd_power', 'ref_power', 'swr', 'mode', 'antenna', 'band', 'radio'].some(key => Object.hasOwn(data, key))) return;
            const now = Date.now();
            if (historyPrevious && now < historyPrevious.wall) clearHistory();
            const radio = data.radio;
            const tciReady = radio?.provider === 'tci' && radio.state === 'ready' &&
                Number.isFinite(radio.age_ms) && radio.age_ms >= 0 && radio.age_ms <= 3000;
            const tx = tciReady && (radio.rx_tx === 'tx' || radio.tuning === true) ? true
                : tciReady && radio.rx_tx === 'rx' && radio.tuning === false ? false
                : Number.isFinite(data.fwd_power) && data.fwd_power > 0;
            const previous = historyPrevious;
            const continuous = previous && now - previous.wall <= historyGap;
            if (historyTxOnly && previous?.tx && continuous) historyElapsed += now - previous.wall;
            if (!continuous || !previous?.tx) historySegment++;
            historyPrevious = { wall: now, tx };
            // Finish the last observed TX interval at its RX edge, then freeze.
            // Do not insert an RX zero or bridge separate transmissions.
            if (historyTxOnly && !tx) {
                if (previous?.tx && continuous && historySamples.length) {
                    storeHistory({ ...historySamples.at(-1), at: historyElapsed, wall: now });
                }
                scheduleHistoryDraw();
                return;
            }
            const power = value => Number.isFinite(value) && value >= 0 ? value : null;
            storeHistory({ at: historyTxOnly ? historyElapsed : now, wall: now, segment: historySegment,
                fwd: power(data.fwd_power), ref: power(data.ref_power),
                swr: Number.isFinite(data.swr) && data.swr >= 1 ? data.swr : null,
                mode: Number.isInteger(data.mode) && data.mode >= 1 && data.mode <= 4 ? data.mode : null,
                antenna: data.antenna === 1 || data.antenna === 2 ? data.antenna : null,
                band: antennaBands.includes(data.band) ? data.band : null, held: data.band_held === true,
                tciBand: tciReady && Number.isSafeInteger(radio.tx_frequency_hz) && radio.tx_frequency_hz > 0 &&
                    antennaBands.includes(radio.band) ? radio.band : null });
            scheduleHistoryDraw();
        }

function storeHistory(sample) {
            historySamples.push(sample);
            const bucket = Math.floor(sample.at / historyBucketMs) * historyBucketMs;
            let first = historySamples.length - 1;
            while (first > 0 && historySamples[first - 1].at >= bucket) first--;
            if (historySamples.length - first > 8) {
                const points = historySamples.slice(first), keep = new Set([0, points.length - 1]);
                for (const key of ['fwd', 'ref', 'swr']) {
                    let low = -1, high = -1;
                    points.forEach((point, index) => {
                        if (point[key] === null) return;
                        if (low < 0 || point[key] < points[low][key]) low = index;
                        if (high < 0 || point[key] > points[high][key]) high = index;
                    });
                    if (low >= 0) { keep.add(low); keep.add(high); }
                }
                const compacted = [], gaps = { fwd: false, ref: false, swr: false };
                points.forEach((point, index) => {
                    for (const key of Object.keys(gaps)) gaps[key] ||= point[key] === null || point.breaks?.[key] === true;
                    if (!keep.has(index)) return;
                    compacted.push({ ...point, breaks: { ...gaps } });
                    for (const key of Object.keys(gaps)) gaps[key] = false;
                });
                historySamples.splice(first, points.length, ...compacted);
            }
            pruneHistory(sample.at);
        }

function setHistoryTxOnly(enabled) {
            if (historyTxOnly === Boolean(enabled)) return;
            historyTxOnly = Boolean(enabled);
            clearHistory();
        }

function setHistoryWindow(seconds) {
            const value = Number(seconds);
            if (![60, 300, 900, 1800].includes(value)) return;
            historyWindow = value * 1000;
            historyCursor = null;
            scheduleHistoryDraw();
        }

function clearHistory() {
            historySamples.length = 0;
            historyPrevious = null; historyElapsed = 0; historySegment = 0;
            historyCursor = null;
            scheduleHistoryDraw();
        }

function historyLabel(sample, key) {
            if (!sample || sample[key] === null) return '--';
            if (key === 'mode') return historyModes[sample.mode];
            if (key === 'antenna') return 'ANT ' + sample.antenna;
            if (key === 'band') return sample.band + (sample.held ? t(' (last)') : '');
            if (key === 'tciBand') return sample.tciBand;
            return sample[key].toFixed(key === 'swr' ? 2 : 1) + (key === 'swr' ? ':1' : t(' W'));
        }

function historyAt(at) {
            if (historyTxOnly && at > historyElapsed) return null;
            // Use the preceding snapshot, never a later value from the future.
            let low = 0, high = historySamples.length;
            while (low < high) {
                const mid = (low + high) >>> 1;
                if (historySamples[mid].at <= at) low = mid + 1;
                else high = mid;
            }
            const sample = historySamples[low - 1];
            return sample && at - sample.at <= historyGap ? sample : null;
        }

let ws = null;

let wsReconnectTimer = null;

let wsMeterGeneration = 0;

let wsLastMeterAt = null;

let httpMeterAt = null;

async function connectWS() {
            // Browsers don't reliably attach Basic Auth to WebSocket upgrade
            // requests, so we fetch a single-use bearer token via plain HTTP
            // (which DOES carry the cached creds) and pass it on the WS URL.
            let token;
            try {
                const r = await fetch('/api/ws-token');
                if (!r.ok) throw new Error('token http ' + r.status);
                ({ token } = await r.json());
            } catch (err) {
                setConnection('Auth failed', 'offline');
                if (!wsReconnectTimer) {
                    wsReconnectTimer = setTimeout(() => {
                        wsReconnectTimer = null;
                        connectWS();
                    }, 3000);
                }
                return;
            }
            const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
            ws = new WebSocket(proto + '//' + location.host + '/api/ws?t=' + token);

            ws.onopen = () => {
                // A reboot starts a fresh revision sequence. Invalidate HTTP
                // responses that were started before this connection opened.
                meterScaleEpoch++;
                meterScaleRevision = null;
                wsLastMeterAt = null;
                setConnection('Connected');
            };

            ws.onmessage = (e) => {
                // Server sends meter JSON as text frames. Schema matches what
                // serializeMeter() produces — fwd_power, ref_power, swr, band,
                // mode, antenna.
                try {
                    updateMeter(JSON.parse(e.data));
                    wsMeterGeneration++;
                    wsLastMeterAt = Date.now();
                    httpMeterAt = null;
                } catch (err) { /* ignore non-JSON frames */ }
            };

            ws.onclose = () => {
                wsMeterGeneration++;
                wsLastMeterAt = null;
                const httpFresh = httpMeterAt !== null && Date.now() - httpMeterAt <= 5000;
                setConnection(httpFresh ? 'Connected (HTTP)' : 'Disconnected', httpFresh ? '' : 'offline');
                // A failing socket must not erase the working HTTP transport.
                if (!httpFresh) updateMeter({ fwd_power: null, ref_power: null, swr: null, band: '--', mode: 0, antenna: 0,
                    antenna_source: 'unknown', radio: { state: 'unavailable', rx_tx: 'unknown', tx_frequency_hz: null } });
                if (!wsReconnectTimer) {
                    wsReconnectTimer = setTimeout(() => {
                        wsReconnectTimer = null;
                        connectWS();
                    }, 2000);
                }
            };

            ws.onerror = () => {
                // onclose fires next; the reconnect happens there.
            };
        }

let commandPending = false;

async function sendCommand(cmd) {
            if (commandPending) return false;
            commandPending = true;
            showToast('Waiting for tuner...', 'pending', true);
            document.querySelectorAll('.button-stack button').forEach(button => button.disabled = true);
            const controller = new AbortController();
            // Allow the full tuner cycle and the display-to-remote HTTP hop.
            const timeout = setTimeout(() => controller.abort(), 30000);
            // WebSocket is non-blocking server-side, so commands no longer
            // need the close-then-reopen dance the SSE implementation used.
            try {
                const resp = await fetch('/api/command/' + cmd, { method: 'POST', signal: controller.signal });
                if (!resp.ok) throw new Error('HTTP ' + resp.status);
                const data = await resp.json();
                if (data.result === 'success') {
                    if (['manual', 'auto', 'controlled'].includes(cmd)) updateOperatingMode(data);
                    const names = { full_tune: 'Full tune completed', memory_tune: 'Memory tune completed',
                        auto: 'Auto mode selected', manual: 'Manual mode selected', controlled: 'Controlled mode selected', bypass: 'Bypass enabled',
                        antenna_1: 'ANT 1 selected', antenna_2: 'ANT 2 selected' };
                    showToast(() => t(names[cmd]) || t('Command completed'));
                } else {
                    showToast('Tuner did not confirm the command', 'error');
                }
            } catch (err) {
                showToast(() => t('Command result unavailable: ') + t(err.message), 'error');
            } finally {
                clearTimeout(timeout);
                commandPending = false;
                document.querySelectorAll('.button-stack button').forEach(button => button.disabled = false);
            }
        }

function mqttErrorText(error) {
            if (!error) return '';
            let text = t(error.message);
            if (error.code !== undefined) text += t(' (code ') + error.code + ')';
            if (error.detail) text += ': ' + t(error.detail);
            if (error.topic) text += t('; topic: ') + error.topic;
            return text;
        }

function updateMqttStatus(status) {
            const labels = { disabled: 'Disabled', waiting_wifi: 'Waiting for Wi-Fi',
                connecting: 'Connecting', connected: 'Connected', disconnected: 'Reconnecting', error: 'Error' };
            const label = status.errorActive ? 'Error' : (labels[status.state] || 'Unknown');
            const dot = status.errorActive || status.state === 'error' ? 'offline'
                : status.state === 'connected' ? ''
                : status.state === 'disabled' ? 'disabled' : 'connecting';
            setText('mqttStatusText', () => 'MQTT: ' + t(label));
            document.getElementById('mqttStatusDot').className = 'status-dot ' + dot;
            setText('mqttStatusMessage', () => t(status.message));
            setText('mqttStatusDetail', () => {
                let detail = status.enabled ? t('Broker: ') + status.broker + ':' + status.port
                    + t(' · Connection attempts: ') + status.attempts : t('Set a broker below to enable MQTT.');
                if (status.connectedForMs !== undefined)
                    detail += t(' · Connected for ') + Math.floor(status.connectedForMs / 1000) + t(' s');
                if (status.state === 'disconnected')
                    detail += t(' · Retry in ') + Math.ceil(status.retryInMs / 1000) + t(' s');
                return detail;
            });
            const error = document.getElementById('mqttStatusError');
            if (error) { error.hidden = !status.lastError; error.className = status.errorActive ? 'mqtt-error' : ''; }
            const errorText = () => status.lastError ? (status.errorActive ? t('Error: ') : t('Last error (recovered): '))
                + mqttErrorText(status.lastError) + ' · ' + Math.floor(status.lastError.ageMs / 1000) + t(' s ago') : '';
            setText(error, errorText);
            setText('mqttIndicator', () => t(status.message)
                + (status.lastError ? '\n' + errorText() : '') + '\n' + t('Open MQTT settings'), 'title');
        }

let mqttStatusPending = false, mqttStatusTimer = null;

async function refreshMqttStatus() {
            if (mqttStatusPending) return;
            mqttStatusPending = true;
            clearTimeout(mqttStatusTimer);
            if (!document.hidden) {
                const epoch = meterScaleEpoch;
                const generation = wsMeterGeneration;
                try {
                    const data = await readJson('/api/status', 0);
                    lastStatusSnapshot = data;
                    // HTTP also carries the complete meter/control snapshot.
                    // Use it if the socket is silent; never replace a newer
                    // WebSocket message with a request that started earlier.
                    if (epoch === meterScaleEpoch && generation === wsMeterGeneration &&
                        (wsLastMeterAt === null || Date.now() - wsLastMeterAt > 1500)) {
                        updateMeter(data);
                        httpMeterAt = Date.now();
                        setConnection('Connected (HTTP)');
                    } else if (applyMeterScale(data, epoch)) drawMeter();
                    if (!data.mqtt) throw new Error('MQTT diagnostics missing; update firmware');
                    updateMqttStatus(data.mqtt);
                    updateBacklightStatus(data.display);
                    renderDeviceRuntime(data);
                } catch (err) {
                    if (httpMeterAt !== null && Date.now() - httpMeterAt > 5000) {
                        httpMeterAt = null;
                        updateMeter({ fwd_power: null, ref_power: null, swr: null, band: '--', mode: 0, antenna: 0,
                            antenna_source: 'unknown', radio: { state: 'unavailable', rx_tx: 'unknown', tx_frequency_hz: null } });
                        setConnection('Disconnected', 'offline');
                    }
                    setText('mqttStatusText', 'MQTT: status unavailable');
                    document.getElementById('mqttStatusDot').className = 'status-dot disabled';
                    setText('mqttStatusMessage', 'Live status unavailable');
                    setText('mqttStatusDetail', () => t('Cannot read device status: ') + t(err.message));
                    if (document.getElementById('mqttStatusError')) document.getElementById('mqttStatusError').hidden = true;
                    setText('mqttIndicator', () => t('Live status unavailable; open MQTT settings'), 'title');
                }
            }
            mqttStatusPending = false;
            mqttStatusTimer = setTimeout(refreshMqttStatus, 3000);
        }

let mqttReconnectPending = false;

async function reconnectMqtt() {
            if (mqttReconnectPending) return;
            mqttReconnectPending = true;
            const button = document.getElementById('btnMqttReconnect');
            button.disabled = true;
            const controller = new AbortController();
            const timeout = setTimeout(() => controller.abort(), 5000);
            try {
                const response = await fetch('/api/mqtt/reconnect', { method: 'POST', signal: controller.signal });
                if (!response.ok || (await response.json()).result !== 'ok') throw new Error('request rejected');
                showToast('MQTT reconnect requested');
                await refreshMqttStatus();
            } catch (error) { showToast(() => t('Cannot reconnect MQTT: ') + t(error.message), 'error'); }
            finally { clearTimeout(timeout); button.disabled = false; mqttReconnectPending = false; }
        }

document.getElementById('mqttIndicator').addEventListener('click', async () => {
            document.querySelector('.nav-tab[data-tab="settings"]').click();
            await loadAdvancedSettings();
            document.getElementById('mqttStatusMessage')?.scrollIntoView({ block: 'center', behavior: 'smooth' });
        });

const antennaBands = ['160m', '80m', '60m', '40m', '30m', '20m', '17m', '15m', '12m', '10m', '6m'];

let radioSettingsLoaded = false, radioSavePending = false;

function updateRadioStatus(radio, source = 'ldg') {
            const names = { disabled: "TCI disabled", disconnected: "TCI disconnected",
                connecting: "TCI connecting", synchronizing: "TCI synchronizing", ready: "TCI connected", unavailable: "Controller unavailable" };
            const trx = radio.rx_tx === 'tx' ? 'TX' : radio.rx_tx === 'rx' ? 'RX' : '?';
            setText('radioStatus', () => (t(names[radio.state]) || t("TCI unknown"))
                + (radio.state !== 'disabled' ? ' · ' + trx + (radio.tuning ? ' TUNE' : '') : '')
                + ' · ' + (source === 'tci' ? 'TCI' : source === 'ldg' ? 'LDG' : '--'));
            setText('radioFrequency', () => Number.isSafeInteger(radio.tx_frequency_hz) && radio.tx_frequency_hz > 0
                ? (radio.tx_frequency_hz / 1e6).toFixed(6) + ' MHz · ' + (radio.band || '--') + (radio.split ? ' · SPLIT' : '') : '--');
            setText('radioDiagnostics', () => [radio.device, radio.protocol, radio.error,
                radio.free_heap ? 'Heap ' + radio.free_heap + ' B; max ' + radio.largest_block + ' B; stack ' + radio.stack_free + ' B' : '']
                .filter(Boolean).join(' · '));
        }

let tuningSettingsLoaded = false;

let tuningSavePending = false;

function validTuningSettings(value) {
            return typeof value.auto_fallback === 'boolean' &&
                Number.isFinite(value.swr_threshold) && value.swr_threshold >= 1.1 && value.swr_threshold <= 10 &&
                Math.abs(value.swr_threshold * 10 - Math.round(value.swr_threshold * 10)) < 0.00001;
        }

async function loadTuningSettings() {
            document.getElementById('btnSaveTuning').disabled = true;
            try {
                const data = await readJson('/api/tuning-config', 1);
                if (!validTuningSettings(data)) throw new Error('settings');
                document.getElementById('tuningAutoFallback').checked = data.auto_fallback;
                document.getElementById('tuningSwrThreshold').value = data.swr_threshold.toFixed(1);
                tuningSettingsLoaded = true;
                document.getElementById('tuningAutoFallback').disabled = false;
                document.getElementById('tuningSwrThreshold').disabled = false;
                document.getElementById('btnSaveTuning').disabled = false;
                setText('tuningSettingsStatus', "Applies without rebooting. MANUAL is restored after temporary AUTO.");
            } catch (error) {
                setText('tuningSettingsStatus', "Cannot load settings. Reload the page.");
            }
        }

async function saveTuningSettings() {
            if (!tuningSettingsLoaded || tuningSavePending) return;
            const value = { auto_fallback: document.getElementById('tuningAutoFallback').checked,
                swr_threshold: Number(document.getElementById('tuningSwrThreshold').value) };
            if (!validTuningSettings(value)) { showToast("SWR threshold: 1.1–10.0 in steps of 0.1", 'error'); return; }
            tuningSavePending = true;
            const button = document.getElementById('btnSaveTuning'); button.disabled = true;
            const controller = new AbortController();
            const timeout = setTimeout(() => controller.abort(), 5000);
            try {
                const response = await fetch('/api/tuning-config', { method: 'POST',
                    headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(value), signal: controller.signal });
                const data = await response.json();
                if (!response.ok || data.result !== 'ok') throw new Error(data.error || 'save');
                showToast("CONTROLLED settings saved");
            } catch (error) { showToast(() => t("Cannot save CONTROLLED settings: ") + t(error.message), 'error'); }
            finally { clearTimeout(timeout); tuningSavePending = false; button.disabled = false; }
        }

let antennaSettingsLoaded = false;

let antennaSavePending = false;

function updateAntennaStatus(data) {
            const tuning = { native: "Native control", waiting_manual: "Confirming MANUAL", waiting_rf: "MANUAL: waiting for a new transmission",
                waiting_antenna: "Waiting for antenna selection", memory_tune: 'MEM TUNE', waiting_swr: "Checking SWR after MEM TUNE",
                matched: "MEM TUNE completed", auto_active: "AUTO until transmission ends", returning_manual: "Returning to MANUAL",
                high_power: "Tuning blocked: power ≥100 W", no_fresh_data: "Waiting for fresh SWR",
                error: "Tuner did not confirm tuning/mode", manual_override: "Cancelled by manual command until the next transmission" };
            if (data.tuning_state !== undefined) setText('tuningStatus', () => t(tuning[data.tuning_state]) || '--');
            const init = { unknown: '--', waiting_meter: "Waiting for data", waiting_rf_off: "Waiting for transmission to end",
                checking: "Checking", restoring: "Restoring the original antenna", ready: "Ready", error: "Check failed" };
            const auto = { disabled: "Switching blocked: automation disabled", idle: "Waiting for a band change", pending: "Selecting antenna",
                selected: "Antenna selected", already_selected: "Already selected", skipped_power: "Switching blocked: power ≥100 W",
                invalid_power: "Switching blocked: no fresh valid power",
                unassigned: "Table specifies “Do not switch”", uninitialized: "Switching blocked: antenna unknown",
                manual_override: "Cancelled by manual command", manual_selection: "User selected the antenna manually",
                initial_band: "First band: original antenna retained", retrying: "Retrying antenna selection",
                waiting_meter: "Waiting for a fresh power reading",
                waiting_radio: "Waiting for complete TCI state and band",
                waiting_rx: "Waiting for RX without TUNE", waiting_band: "Waiting for a stable TCI band",
                tuning: "Antenna selected; MEM TUNE in progress", tune_failed: "Antenna selected; MEM TUNE unconfirmed",
                tune_no_rf: "Antenna selected; MEM TUNE skipped: no transmission",
                tune_power_limit: "Antenna selected; MEM TUNE blocked: power ≥100 W",
                tune_invalid_power: "Antenna selected; MEM TUNE skipped: no fresh valid power",
                tune_band_changed: "Antenna selected; MEM TUNE cancelled: band changed",
                tune_manual_override: "Antenna selected; MEM TUNE cancelled by manual command",
                waiting_tuner: "Tuner did not confirm MANUAL: selection saved, waiting for retry or transmission end",
                error: "Tuner did not confirm selection", waiting_rf_off: "Mode unknown: waiting for transmission to end" };
            if (data.antenna_init !== undefined)
                setText('antennaInitStatus', () => t(init[data.antenna_init]) || '--');
            if (data.antenna_auto !== undefined) {
                setText('antennaAutoStatus', () => {
                    let text = t(auto[data.antenna_auto]) || '--';
                    if ((data.antenna_auto === 'selected' || data.antenna_auto === 'already_selected') &&
                        antennaBands.includes(data.antenna_auto_band)) {
                        text += t(' for ') + data.antenna_auto_band;
                        if (data.antenna_source === 'tci') text += ' · TCI';
                        else if (data.band_held === true) text += t('; no fresh tuner data');
                        else if (data.band && data.band !== data.antenna_auto_band)
                            text += t('; waiting for band confirmation: ') + data.band;
                    }
                    return text;
                });
            }
        }

async function loadAntennaSettings() {
    try {
        const data = await readJson('/api/antenna-config', 1);
        if (typeof data.enabled !== 'boolean' || !Array.isArray(data.bands) || data.bands.length !== antennaBands.length ||
            !data.bands.every(value => Number.isInteger(value) && value >= 0 && value <= 2)) throw new Error('table');
        antennaSettingsSnapshot = { enabled: data.enabled, bands: data.bands.slice() };
        document.getElementById('antennaAutoEnabled').checked = data.enabled;
        document.getElementById('antennaAutoEnabled').disabled = false;
        document.getElementById('btnSaveAutomation').disabled = false;
        antennaSettingsLoaded = true;
        settingsModule?.applyAntennaTable();
        setText('antennaSettingsStatus', 'Changes apply at the next band change.');
    } catch (_) { setText('antennaSettingsStatus', 'Cannot load table. Reload the page.'); }
}

let displayOrientationLoaded = false, displayOrientationPending = false;

function renderDeviceRuntime(data) {
setText('deviceRuntime', () => Number.isFinite(data.uptime_ms)
                        ? t('Device uptime: ') + Math.floor(data.uptime_ms / 1000) + t(' s')
                          + (data.firmware_build ? t(' · Build: ') + data.firmware_build : '') : '');
}
