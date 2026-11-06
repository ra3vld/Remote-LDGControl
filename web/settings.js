function toggleStaticIP() {
            const on = document.getElementById('useStaticIP').checked;
            document.getElementById('staticIPFields').style.display = on ? '' : 'none';
        }

function updateBrightnessControls() {
            document.getElementById('brightnessManual').disabled = document.getElementById('brightnessAuto').checked;
        }

function calibrateBrightness(id) {
            if (!brightnessLoaded || lightRaw === null || Date.now() - lightReadAt > 10000) {
                showToast("No fresh sensor data", 'error'); return;
            }
            document.getElementById(id).value = String(lightRaw);
        }

async function loadBrightnessSettings() {
            try {
                const data = await readJson('/api/display-config', 2);
                updateBacklightStatus(data);
                if (!data.supported) return;
                const ids = { manual: 'brightnessManual', minimum: 'brightnessMin', maximum: 'brightnessMax',
                    dark_adc: 'brightnessDark', bright_adc: 'brightnessBright' };
                for (const [key, id] of Object.entries(ids)) document.getElementById(id).value = String(data[key]);
                document.getElementById('brightnessAuto').checked = data.automatic;
                updateBrightnessControls();
                brightnessLoaded = true;
                document.getElementById('btnSaveBrightness').disabled = false;
            } catch (err) { setText('brightnessStatus', "Cannot load brightness settings"); }
        }

async function saveBrightnessSettings() {
            if (!brightnessLoaded || brightnessPending) return;
            const settings = { automatic: document.getElementById('brightnessAuto').checked,
                manual: Number(document.getElementById('brightnessManual').value),
                minimum: Number(document.getElementById('brightnessMin').value),
                maximum: Number(document.getElementById('brightnessMax').value),
                dark_adc: Number(document.getElementById('brightnessDark').value),
                bright_adc: Number(document.getElementById('brightnessBright').value) };
            if (![settings.manual, settings.minimum, settings.maximum].every(n => Number.isInteger(n) && n >= 1 && n <= 100) ||
                settings.minimum > settings.maximum ||
                ![settings.dark_adc, settings.bright_adc].every(n => Number.isInteger(n) && n >= 0 && n <= 4095) ||
                Math.abs(settings.dark_adc - settings.bright_adc) < 100) {
                showToast("Check brightness 1–100%, minimum ≤ maximum and calibration difference of at least 100", 'error'); return;
            }
            brightnessPending = true;
            const button = document.getElementById('btnSaveBrightness'); button.disabled = true;
            const controller = new AbortController();
            const timeout = setTimeout(() => controller.abort(), 5000);
            try {
                const resp = await fetch('/api/display-config', { method: 'POST', signal: controller.signal,
                    headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(settings) });
                if (!resp.ok || (await resp.json()).result !== 'ok') throw new Error('save');
                showToast("Brightness settings saved");
            } catch (err) { showToast("Cannot save brightness", 'error'); }
            finally { clearTimeout(timeout); brightnessPending = false; button.disabled = false; }
        }

document.getElementById('antennaBandTable').innerHTML = antennaBands.map(band =>
            '<div class="form-group"><label for="antennaBand' + band + '">' + band + '</label>' +
            '<select id="antennaBand' + band + '"><option value="0" data-i18n>Do not switch</option>' +
            '<option value="1">ANT1</option><option value="2">ANT2</option></select></div>').join('');

async function loadRadioSettings() {
            try {
                const data = await readJson('/api/radio-config', 2);
                if (typeof data.enabled !== 'boolean' || typeof data.host !== 'string' ||
                    !Number.isInteger(data.port) || data.port < 1 || data.port > 65535 ||
                    !Number.isInteger(data.receiver) || data.receiver < 0 || data.receiver > 15) throw new Error('config');
                document.getElementById('radioEnabled').checked = data.enabled;
                document.getElementById('radioHost').value = data.host;
                document.getElementById('radioPort').value = String(data.port);
                document.getElementById('radioReceiver').value = String(data.receiver);
                radioSettingsLoaded = true;
                document.getElementById('btnSaveRadio').disabled = false;
                setText('radioSettingsStatus', "TCI settings apply without rebooting. The RX index must match the transmitter in use.");
            } catch (error) { setText('radioSettingsStatus', "Cannot load TCI settings. Reload the page."); }
        }

async function saveRadioSettings() {
            if (!radioSettingsLoaded || radioSavePending) return;
            const config = { enabled: document.getElementById('radioEnabled').checked,
                host: document.getElementById('radioHost').value.trim(),
                port: Number(document.getElementById('radioPort').value), receiver: Number(document.getElementById('radioReceiver').value) };
            if ((config.enabled && !config.host) || config.host.length > 63 || !/^[a-zA-Z0-9.-]*$/.test(config.host) ||
                !Number.isInteger(config.port) || config.port < 1 || config.port > 65535 ||
                !Number.isInteger(config.receiver) || config.receiver < 0 || config.receiver > 15) {
                showToast("Check the address, port and RX index", 'error'); return;
            }
            radioSavePending = true;
            const button = document.getElementById('btnSaveRadio'); button.disabled = true;
            const controller = new AbortController();
            const timeout = setTimeout(() => controller.abort(), 5000);
            try {
                const response = await fetch('/api/radio-config', { method: 'POST', headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify(config), signal: controller.signal });
                const data = await response.json();
                if (!response.ok || data.result !== 'ok') throw new Error(data.error || 'save');
                showToast("TCI settings saved");
            } catch (error) { showToast(() => t("Cannot save TCI: ") + t(error.message), 'error'); }
            finally { clearTimeout(timeout); radioSavePending = false; button.disabled = false; }
        }

async function saveAntennaSettings() {
            if (!antennaSettingsLoaded || antennaSavePending) return;
            const config = { enabled: document.getElementById('antennaAutoEnabled').checked,
                bands: antennaBands.map(band => Number(document.getElementById('antennaBand' + band).value)) };
            if (!config.bands.every(value => Number.isInteger(value) && value >= 0 && value <= 2)) return;
            antennaSavePending = true;
            const button = document.getElementById('btnSaveAntennas');
            button.disabled = true;
            const controller = new AbortController();
            const timeout = setTimeout(() => controller.abort(), 5000);
            try {
                const resp = await fetch('/api/antenna-config', { method: 'POST',
                    headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(config), signal: controller.signal });
                const data = await resp.json();
                if (!resp.ok || data.result !== 'ok') throw new Error('save');
                antennaSettingsSnapshot = { enabled: config.enabled, bands: config.bands.slice() };
                showToast("Antenna table saved");
            } catch (err) { showToast("Cannot save antenna table", 'error'); }
            finally { clearTimeout(timeout); button.disabled = false; antennaSavePending = false; }
        }

function readDisplayOrientation() {
            const rotation = Number(document.getElementById('displayRotation').value);
            if (!Number.isInteger(rotation) || rotation < 0 || rotation > 3) return null;
            return rotation | (document.getElementById('displayMirrorX').checked ? 4 : 0)
                | (document.getElementById('displayMirrorY').checked ? 8 : 0);
        }

async function saveDisplayOrientation() {
            if (!displayOrientationLoaded || displayOrientationPending) return;
            const orientation = readDisplayOrientation();
            if (orientation === null) return;
            displayOrientationPending = true;
            const button = document.getElementById('btnSaveOrientation'); button.disabled = true;
            const controller = new AbortController();
            const timeout = setTimeout(() => controller.abort(), 5000);
            try {
                const response = await fetch('/api/config', { method: 'POST', signal: controller.signal,
                    headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ display_orientation: orientation }) });
                const data = await response.json();
                if (!response.ok || data.result !== 'ok') throw new Error('save');
                setText('orientationStatus', 'Display orientation saved');
                showToast('Display orientation saved');
            } catch (error) { showToast('Cannot save display orientation', 'error'); }
            finally { clearTimeout(timeout); button.disabled = false; displayOrientationPending = false; }
        }

function applySettingsForm(data) {
    document.getElementById("btnSaveGeneral").disabled = false;

                displayOrientationLoaded = data.build === 'display' && Number.isInteger(data.display_orientation)
                    && data.display_orientation >= 0 && data.display_orientation < 16;
                document.getElementById('orientationControls').disabled = !displayOrientationLoaded;
                setText('orientationStatus', displayOrientationLoaded ? 'Display orientation ready'
                    : 'Display orientation is unavailable on this firmware');
                if (displayOrientationLoaded) {
                    document.getElementById('displayRotation').value = String(data.display_orientation & 3);
                    document.getElementById('displayMirrorX').checked = !!(data.display_orientation & 4);
                    document.getElementById('displayMirrorY').checked = !!(data.display_orientation & 8);
                }
                if (data.mqttBroker) document.getElementById('mqttBroker').value = data.mqttBroker;
                if (data.mqttPort) document.getElementById('mqttPort').value = data.mqttPort;
                if (data.mqttUsername) document.getElementById('mqttUser').value = data.mqttUsername;
                document.getElementById('mqttDiscoveryEnabled').checked = data.mqttDiscoveryEnabled !== false;
                document.getElementById('mqttBaseTopic').value = data.mqttBaseTopic || '';
                if (data.webUsername) document.getElementById('webUser').value = data.webUsername;
                if (data.meterPsuVoltage) document.getElementById('psuVoltage').value = data.meterPsuVoltage;
                if (data.ant1Name) document.getElementById('ant1Name').value = data.ant1Name;
                if (data.ant2Name) document.getElementById('ant2Name').value = data.ant2Name;
                if (data.ant1Name) document.getElementById('ant1Label').textContent = data.ant1Name;
                if (data.ant2Name) document.getElementById('ant2Label').textContent = data.ant2Name;
                if (data.wifiSSID) document.getElementById('ssid').value = data.wifiSSID;
                document.getElementById('useStaticIP').checked = !!data.useStaticIP;
                toggleStaticIP();
                if (data.staticIP)      document.getElementById('staticIP').value      = data.staticIP;
                if (data.staticNetmask) document.getElementById('staticNetmask').value = data.staticNetmask;
                if (data.staticGateway) document.getElementById('staticGateway').value = data.staticGateway;
                if (data.staticDNS)     document.getElementById('staticDNS').value     = data.staticDNS;
                if (data.currentIP)     document.getElementById('currentIP').textContent     = data.currentIP;
                if (data.currentNetmask)document.getElementById('currentNetmask').textContent= data.currentNetmask;
                if (data.currentGateway)document.getElementById('currentGateway').textContent= data.currentGateway;
                // Pre-fill static fields with DHCP values if not already set
                if (!data.useStaticIP) {
                    if (!data.staticIP)      document.getElementById('staticIP').value      = data.currentIP      || '';
                    if (!data.staticNetmask) document.getElementById('staticNetmask').value = data.currentNetmask || '';
                    if (!data.staticGateway) document.getElementById('staticGateway').value = data.currentGateway || '';
                    if (!data.staticDNS)     document.getElementById('staticDNS').value     = data.currentDNS     || '';
                }
                // Show Remote Unit Host only on display build
                if (data.build === 'display') {
                    document.getElementById('remoteHostGroup').style.display = '';
                    if (data.remoteHost) document.getElementById('remoteHost').value = data.remoteHost;
                }

}

async function saveSettings() {
            // The main settings button must also save edits in the scale form.
            if (meterSettingsEdited() && !(await saveMeterSettings())) return;
            const config = {
                mqttBroker: document.getElementById('mqttBroker').value,
                mqttPort: parseInt(document.getElementById('mqttPort').value),
                mqttUsername: document.getElementById('mqttUser').value,
                mqttPassword: document.getElementById('mqttPass').value,
                mqttDiscoveryEnabled: document.getElementById('mqttDiscoveryEnabled').checked,
                webUsername: document.getElementById('webUser').value,
                webPassword: document.getElementById('webPass').value,
                meterPsuVoltage: parseFloat(document.getElementById('psuVoltage').value),
                ant1Name: document.getElementById('ant1Name').value,
                ant2Name: document.getElementById('ant2Name').value,
                useStaticIP:    document.getElementById('useStaticIP').checked,
                staticIP:       document.getElementById('staticIP').value,
                staticNetmask:  document.getElementById('staticNetmask').value,
                staticGateway:  document.getElementById('staticGateway').value,
                staticDNS:      document.getElementById('staticDNS').value,
                remoteHost:     document.getElementById('remoteHost').value
            };

            try {
                const resp = await fetch('/api/config', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify(config)
                });
                const data = await resp.json();
                if (data.result === 'ok') {
                    showToast(() => data.restart ? t('Settings saved. Restarting...') : t('Settings saved'));
                } else {
                    showToast('Save failed', 'error');
                }
            } catch (err) {
                showToast('Connection error', 'error');
            }
        }

async function resetSettings() {
            if (!confirm(t('Reset all settings to defaults? This will restart the device.'))) return;
            try {
                await fetch('/api/config', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ reset: true })
                });
                showToast('Settings reset. Restarting...');
            } catch (err) {
                showToast('Reset failed', 'error');
            }
        }

function applyAntennaTable() {
    document.getElementById('btnSaveAntennas').disabled = !antennaSettingsLoaded;
    antennaBands.forEach((band, i) => {
        const element = document.getElementById('antennaBand' + band);
        element.disabled = !antennaSettingsLoaded;
        if (antennaSettingsSnapshot) element.value = String(antennaSettingsSnapshot.bands[i]);
    });
}
