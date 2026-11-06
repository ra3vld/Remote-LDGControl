const assert = require('node:assert/strict');
const vm = require('node:vm');
const { board, page } = require('./test_meter_scale_web.cjs');

(async () => {
    const values = new Map();
    const storage = { getItem: key => values.get(key), setItem: (key, value) => values.set(key, value) };
    const server = board();
    const ui = await page(server, true, { language: 'ru-RU', storage });
    assert.equal(ui.context.document.documentElement.lang, 'ru');
    assert.equal(ui.element('connectionText').textContent, 'Подключено');
    ui.socket.message({ band: '20m', band_held: true, antenna_init: 'ready',
        antenna_auto: 'selected', antenna_auto_band: '20m', tuning_state: 'waiting_rf',
        radio: { state: 'ready', rx_tx: 'rx', tx_frequency_hz: 14200000 } });
    assert.equal(ui.element('antennaInitStatus').textContent, 'Готово');
    ui.element('mqttBroker').value = 'Draft';
    ui.element('meterRange').value = '42';
    const before = server.posts;
    vm.runInContext("showToast('Waiting for tuner...', 'pending', true); setLanguage('en')", ui.context);
    assert.equal(ui.element('toast').textContent, 'Waiting for tuner...');
    assert.equal(ui.element('connectionText').textContent, 'Connected');
    assert.equal(ui.element('antennaInitStatus').textContent, 'Ready');
    assert.equal(ui.element('antennaAutoStatus').textContent, 'Antenna selected for 20m; no fresh tuner data');
    assert.equal(ui.element('bandValue').textContent, 'Band: 20m (last)');
    assert.equal(ui.element('tuningStatus').textContent, 'MANUAL: waiting for a new transmission');
    assert.equal(ui.element('mqttBroker').value, 'Draft');
    assert.equal(ui.element('meterRange').value, '42');
    assert.equal(server.posts, before);
    assert.equal(values.get('ldg.language'), 'en');
    const reopened = await page(board(), true, { language: 'ru-RU', storage });
    assert.equal(reopened.context.document.documentElement.lang, 'en', 'Saved language overrides browser locale');
    values.set('ldg.language', 'invalid');
    const fallback = await page(board(), true, { language: 'ru-RU', storage });
    assert.equal(fallback.context.document.documentElement.lang, 'ru');
    vm.runInContext("setLanguage('ru')", ui.context);
    assert.equal(ui.element('toast').textContent, 'Ожидание тюнера…');
    assert.equal(ui.element('bandValue').textContent, 'Диапазон: 20m (последний)');
    console.log('PASS: RU/EN live snapshot, pending toast, draft preservation, locale, persistence and invalid preference');
})().catch(error => { console.error(error); process.exitCode = 1; });
