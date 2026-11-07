// Core state never depends on a renderer or an optional settings form.
let coreReady = false, localeReady = false;
let visualModule = null, settingsModule = null;
let lastConfig = null, lastStatusSnapshot = null, antennaSettingsSnapshot = null;
let startupPromise = null;
let activeRead = null, interactiveActions = 0;
const pendingReads = [];
const assetLoads = new Map(), loadingScripts = new Map();
let assetTail = Promise.resolve();

function refreshLanguage() {
    document.documentElement.lang = language;
    document.getElementById('languageSelect').value = language;
    for (const [element, properties] of languageBindings)
        for (const [property, render] of properties) element[property] = render();
    drawMeter();
    scheduleHistoryDraw();
}

function readJson(url, priority = 1) {
    return new Promise((resolve, reject) => {
        pendingReads.push({ url, priority, resolve, reject });
        pumpReads();
    });
}

async function pumpReads() {
    if (activeRead || interactiveActions || !pendingReads.length) return;
    pendingReads.sort((a, b) => a.priority - b.priority);
    const item = pendingReads.shift();
    activeRead = item;
    item.controller = new AbortController();
    item.interrupted = false;
    const deadline = setTimeout(() => item.controller.abort(), 5000);
    try {
        const response = await fetch(item.url, { cache: 'no-store', signal: item.controller.signal });
        if (!response.ok) throw new Error('HTTP ' + response.status);
        const data = await response.json();
        if (item.interrupted) pendingReads.push(item);
        else item.resolve(data);
    } catch (error) {
        if (item.interrupted) pendingReads.push(item);
        else item.reject(error);
    } finally {
        clearTimeout(deadline);
        activeRead = null;
        pumpReads();
    }
}

async function interactive(action) {
    interactiveActions++;
    if (activeRead) {
        activeRead.interrupted = true;
        activeRead.controller.abort();
    }
    try { return await action(); }
    finally { interactiveActions--; pumpReads(); }
}

function drawMeter() { callVisual('drawMeter'); }
function drawHistory() { callVisual('drawHistory'); }
function scheduleHistoryDraw() { callVisual('scheduleHistoryDraw'); }
function callVisual(method) {
    if (!visualModule) return;
    try { return visualModule[method](); }
    catch (_) { failVisuals(); }
}
function failVisuals() {
    const previous = visualModule;
    visualModule = null;
    try { previous?.stop(); } catch (_) {}
    assetLoads.delete('widgets');
    document.getElementById('visualLoadStatus').hidden = false;
    document.getElementById('meterCanvas').hidden = true;
    setText('visualLoadStatus', 'Cannot load visuals. Numeric readings and controls remain available.');
    document.getElementById('retryVisuals').hidden = false;
}

function assetIsCurrent(name) {
    return loadingScripts.get(name) === document.currentScript;
}

function loadAsset(name) {
    if (assetLoads.has(name)) return assetLoads.get(name);
    const ready = startupPromise;
    const promise = assetTail.catch(() => {}).then(() => ready).then(() => new Promise((resolve, reject) => {
        const specification = assetManifest[name];
        const script = document.createElement('script');
        let settled = false;
        const finish = error => {
            if (settled) return;
            settled = true;
            clearTimeout(deadline);
            if (loadingScripts.get(name) === script) loadingScripts.delete(name);
            script.remove();
            if (error) reject(error); else resolve();
        };
        const deadline = setTimeout(() => finish(new Error('asset timeout')), 8000);
        script.src = '/?asset=' + specification.file;
        script.integrity = specification.integrity;
        script.onload = () => finish(null);
        script.onerror = () => finish(new Error('asset unavailable'));
        loadingScripts.set(name, script);
        document.head.appendChild(script);
    })).then(() => {
        if (name === 'widgets' && !visualModule) throw new Error('visual initialization failed');
        if (name === 'settings' && !settingsModule) throw new Error('settings initialization failed');
        if (name === 'ru' && !localeReady) throw new Error('locale initialization failed');
    }).catch(error => { assetLoads.delete(name); throw error; });
    assetLoads.set(name, promise);
    assetTail = promise;
    return promise;
}

function ensureLocale() {
    return language === 'ru' && !localeReady ? loadAsset('ru') : Promise.resolve();
}

async function loadVisuals() {
    if (visualModule) return;
    document.getElementById('retryVisuals').hidden = true;
    setText('visualLoadStatus', 'Loading visuals…');
    try { await loadAsset('widgets'); }
    catch (_) { failVisuals(); }
}

function installVisuals(api) {
    visualModule = api;
    document.getElementById('historyTxOnly').checked = historyTxOnly;
    document.getElementById('meterCanvas').hidden = false;
    document.getElementById('meterLoading').hidden = true;
    document.getElementById('visualLoadStatus').hidden = true;
    document.getElementById('retryVisuals').hidden = true;
    drawMeter();
    scheduleHistoryDraw();
}

async function loadAdvancedSettings() {
    const details = document.getElementById('advancedSettings');
    details.open = true;
    if (settingsModule) return;
    document.getElementById('retrySettings').hidden = true;
    setText('settingsLoadStatus', 'Loading other settings…');
    try {
        // Language failure must not prevent loading a usable form.
        await ensureLocale().catch(() => {});
        await loadAsset('settings');
    } catch (_) {
        document.getElementById('advancedSettingsBody').disabled = true;
        document.getElementById('retrySettings').hidden = false;
        setText('settingsLoadStatus', 'Cannot load other settings. Controls remain available.');
    }
}

function installSettings(api) {
    for (const [name, method] of Object.entries(api)) {
        if (name.startsWith('save') || name === 'resetSettings')
            globalThis[name] = (...args) => interactive(() => method(...args));
        else globalThis[name] = method;
    }
    if (lastConfig) api.applySettingsForm(lastConfig);
    api.applyAntennaTable();
    if (lastStatusSnapshot) {
        if (lastStatusSnapshot.mqtt) updateMqttStatus(lastStatusSnapshot.mqtt);
        updateBacklightStatus(lastStatusSnapshot.display);
        renderDeviceRuntime(lastStatusSnapshot);
    }
    bindLanguageText(document.getElementById('advancedSettingsBody'));
    settingsModule = api;
    document.getElementById('advancedSettingsBody').disabled = false;
    setText('settingsLoadStatus', '');
    api.loadRadioSettings().then(api.loadBrightnessSettings);
}

async function loadSettings() {
    try {
        const data = await readJson('/api/config');
        lastConfig = data;
        if (data.ant1Name) document.getElementById('ant1Label').textContent = data.ant1Name;
        if (data.ant2Name) document.getElementById('ant2Label').textContent = data.ant2Name;
        settingsModule?.applySettingsForm(data);
    } catch (_) { setText('criticalSettingsStatus', 'Cannot load configuration. Reload the page.'); }
}

async function saveAntennaAutomation() {
    if (!antennaSettingsLoaded || antennaSavePending) return;
    antennaSavePending = true;
    const button = document.getElementById('btnSaveAutomation'); button.disabled = true;
    try {
        await interactive(async () => {
            const controller = new AbortController();
            const deadline = setTimeout(() => controller.abort(), 5000);
            const candidate = { enabled: document.getElementById('antennaAutoEnabled').checked,
                bands: antennaSettingsSnapshot.bands.slice() };
            try {
                const response = await fetch('/api/antenna-config', { method: 'POST', signal: controller.signal,
                    headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(candidate) });
                if (!response.ok || (await response.json()).result !== 'ok') throw new Error('save');
                antennaSettingsSnapshot = candidate;
                showToast('Antenna table saved');
            } finally { clearTimeout(deadline); }
        });
    } catch (_) { showToast('Cannot save antenna table', 'error'); }
    finally { antennaSavePending = false; button.disabled = false; }
}

async function loadInitialSettings() {
    // Establish current state before any saved configuration or optional code.
    await refreshMqttStatus();
    await loadSettings();
    await loadTuningSettings();
    await loadAntennaSettings();
    coreReady = true;
}
