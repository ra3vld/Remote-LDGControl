const assert = require('node:assert/strict');
const vm = require('node:vm');
const { board, page, tick, deferred } = require('./test_meter_scale_web.cjs');

(async () => {
    const server = board();
    const original = server.fetch;
    server.radio = { enabled: false, host: '', port: 40001, receiver: 0 };
    server.radioPosts = 0;
    server.fetch = async function(url, options = {}) {
        if (url !== '/api/radio-config') return original.call(this, url, options);
        if (options.method === 'POST') {
            this.radioPosts++;
            if (this.holdRadio) await this.holdRadio.promise;
            if (this.failRadio) return { ok: false, json: async () => ({ error: 'save failed' }) };
            this.radio = JSON.parse(options.body);
            return { ok: true, json: async () => ({ result: 'ok', restart: false }) };
        }
        return { ok: true, json: async () => ({ ...this.radio }) };
    };
    const ui = await page(server);
    assert.equal(ui.element('radioPort').value, '40001');
    assert.equal(ui.element('btnSaveRadio').disabled, false);
    ui.element('radioEnabled').checked = true;
    ui.element('radioHost').value = '192.0.2.20';
    ui.element('radioReceiver').value = '1';
    server.holdRadio = deferred();
    const save = vm.runInContext('saveRadioSettings()', ui.context);
    await tick();
    await vm.runInContext('saveRadioSettings()', ui.context);
    assert.equal(server.radioPosts, 1);
    assert.equal(ui.element('btnSaveRadio').disabled, true);
    server.holdRadio.resolve(); await save; delete server.holdRadio;
    assert.deepEqual(server.radio, { enabled: true, host: '192.0.2.20', port: 40001, receiver: 1 });
    assert.doesNotMatch(ui.element('toast').textContent, /restart/i);
    const reload = await page(server);
    assert.equal(reload.element('radioHost').value, '192.0.2.20');
    assert.equal(reload.element('radioEnabled').checked, true);
    ui.element('radioHost').value = 'ws://host';
    await vm.runInContext('saveRadioSettings()', ui.context);
    assert.equal(server.radioPosts, 1, 'Invalid host must not be submitted');
    ui.element('radioHost').value = 'other-host'; server.failRadio = true;
    await vm.runInContext('saveRadioSettings()', ui.context);
    assert.equal(server.radio.host, '192.0.2.20', 'Failed save preserves configuration');
    assert.match(ui.element('toast').textContent, /save failed/);
    assert.equal(ui.element('btnSaveRadio').disabled, false);
    server.broadcast({ ...server.snapshot(), antenna_source: 'tci', antenna_auto: 'waiting_rx',
        radio: { state: 'ready', tx_frequency_hz: 7100200, band: '40m', rx_tx: 'tx', split: true,
            device: '<test>', protocol: 'ExpertSDR3,2.1', free_heap: 120000, largest_block: 80000, stack_free: 3000 } });
    assert.match(ui.element('radioFrequency').textContent, /7\.100200 MHz.*40m.*SPLIT/);
    assert.match(ui.element('radioStatus').textContent, /TX.*TCI/);
    assert.match(ui.element('antennaAutoStatus').textContent, /RX/);
    assert.match(ui.element('radioDiagnostics').textContent, /<test>.*120000/);
    server.broadcast({ ...server.snapshot(), antenna_source: 'ldg',
        radio: { state: 'disconnected', tx_frequency_hz: null, rx_tx: 'unknown', error: 'TCI disconnected' } });
    assert.equal(ui.element('radioFrequency').textContent, '--');
    assert.match(ui.element('radioStatus').textContent, /нет связи.*LDG/);
    console.log('PASS: TCI settings persistence, validation, failed save, duplicate clicks, split/TX status and LDG fallback');
})().catch(error => { console.error(error); process.exitCode = 1; });
