const assert = require('node:assert/strict');
const vm = require('node:vm');
const { board, page, tick, deferred } = require('./test_meter_scale_web.cjs');

(async () => {
    const server = board();
    const fetch = server.fetch;
    const writes = [];
    server.antennaSettings = { enabled: true, bands: [0, 0, 0, 1, 0, 2, 0, 0, 0, 0, 0] };
    server.fetch = async function(url, options = {}) {
        if (url !== '/api/antenna-config') return fetch.call(this, url, options);
        if (options.method !== 'POST') return { ok: true, json: async () => this.antennaSettings };
        writes.push(JSON.parse(options.body));
        if (this.holdSave) await this.holdSave.promise;
        if (this.failSave) return { ok: false, json: async () => ({ error: 'save failed' }) };
        this.antennaSettings = writes.at(-1);
        return { ok: true, json: async () => ({ result: 'ok', restart: false }) };
    };
    const ui = await page(server);
    assert.equal(ui.element('antennaAutoEnabled').checked, true);
    assert.equal(ui.element('antennaBand40m').value, '1');
    assert.equal(ui.element('antennaBand20m').value, '2');
    ui.element('antennaBand20m').value = '1';
    server.holdSave = deferred();
    const save = vm.runInContext('saveAntennaSettings()', ui.context);
    await tick();
    await vm.runInContext('saveAntennaSettings()', ui.context);
    assert.equal(writes.length, 1, 'Duplicate save must not overwrite or run tuner commands');
    assert.equal(ui.element('btnSaveAntennas').disabled, true);
    server.holdSave.resolve(); await save; delete server.holdSave;
    assert.equal(writes[0].enabled, true);
    assert.equal(writes[0].bands[5], 1);
    assert.equal(ui.element('toast').textContent, 'Antenna table saved');
    assert.equal(ui.element('btnSaveAntennas').disabled, false);
    server.failSave = true;
    await vm.runInContext('saveAntennaSettings()', ui.context);
    assert.equal(ui.element('toast').textContent, 'Cannot save antenna table');
    assert.equal(ui.element('btnSaveAntennas').disabled, false);
    ui.socket.message({ antenna_init: 'ready', antenna_auto: 'skipped_power' });
    assert.equal(ui.element('antennaInitStatus').textContent, 'Ready');
    assert.match(ui.element('antennaAutoStatus').textContent, /100 W/);
    ui.socket.message({ antenna_init: 'error', antenna_auto: 'manual_override' });
    assert.equal(ui.element('antennaInitStatus').textContent, 'Check failed');
    assert.equal(ui.element('antennaAutoStatus').textContent, 'Cancelled by manual command');
    ui.socket.message({ antenna_init: 'ready', antenna_auto: 'manual_selection' });
    assert.equal(ui.element('antennaAutoStatus').textContent, 'User selected the antenna manually');
    ui.socket.message({ antenna_auto: 'unassigned' });
    assert.match(ui.element('antennaAutoStatus').textContent, /Do not switch/);
    ui.socket.message({ antenna_auto: 'disabled' });
    assert.match(ui.element('antennaAutoStatus').textContent, /Switching blocked/);
    ui.socket.message({ antenna_auto: 'retrying' });
    assert.equal(ui.element('antennaAutoStatus').textContent, 'Retrying antenna selection');
    ui.socket.message({ antenna_auto: 'waiting_meter' });
    assert.equal(ui.element('antennaAutoStatus').textContent, 'Waiting for a fresh power reading');
    for (const [state, message] of [
        ['waiting_tuner', /selection saved/],
        ['tuning', /MEM TUNE in progress/], ['tune_failed', /MEM TUNE unconfirmed/],
        ['tune_no_rf', /no transmission/], ['tune_power_limit', /power ≥100 W/],
        ['tune_invalid_power', /no fresh valid power/], ['tune_band_changed', /band changed/],
        ['tune_manual_override', /cancelled by manual command/]
    ]) {
        ui.socket.message({ antenna_auto: state });
        assert.match(ui.element('antennaAutoStatus').textContent, message);
    }
    ui.socket.message({ antenna_auto: 'selected', antenna_auto_band: '20m', band: '20m', band_held: true });
    assert.equal(ui.element('antennaAutoStatus').textContent, 'Antenna selected for 20m; no fresh tuner data');
    ui.socket.message({ antenna_auto: 'selected', antenna_auto_band: '20m', band: '40m', band_held: false });
    assert.match(ui.element('antennaAutoStatus').textContent, /for 20m; waiting for band confirmation: 40m/);
    delete server.failSave;
    const reload = await page(server);
    assert.equal(reload.element('antennaBand20m').value, '1');
    console.log('PASS: antenna table load/save/reload, duplicate click, failed persistence and confirmed initialization feedback');
})().catch(error => { console.error(error); process.exitCode = 1; });
