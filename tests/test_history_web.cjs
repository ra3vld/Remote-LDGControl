// Exercise browser history using the production page and its existing transports.
const assert = require('node:assert/strict');
const vm = require('node:vm');
const { board, page, deferred, tick } = require('./test_meter_scale_web.cjs');

(async () => {
    const server = board();
    const ui = await page(server);
    const run = source => vm.runInContext(source, ui.context);
    const snapshot = { fwd_power: 50, ref_power: 2, swr: 1.5, mode: 4, tuner_mode: 2,
        antenna: 2, band: '20m', band_held: false };
    run('clearHistory()');
    const requests = [];
    ui.context.fetch = (...args) => { requests.push(args); return server.fetch(...args); };
    ui.socket.message(snapshot);
    assert.equal(run('historySamples.length'), 1);
    assert.equal(run('historySamples[0].fwd'), 50);
    assert.equal(run('historySamples[0].mode'), 4, 'History records the selected operating mode');
    assert.equal(run('historySamples[0].antenna'), 2);
    const firstAt = run('historySamples[0].at');
    assert.equal(run(`historyAt(${firstAt - 1})`), null, 'Never use a future snapshot');
    assert.equal(run(`historyAt(${firstAt + 5001})`), null, 'Never extend stale telemetry');
    ui.socket.message({ meter_scale: 100, meter_scale_revision: 1 });
    assert.equal(run('historySamples.length'), 1, 'Scale metadata is not a new measurement');

    await ui.advance(500);
    ui.socket.message({ ...snapshot, fwd_power: 0, ref_power: 0, swr: null, antenna: 0, band_held: true });
    assert.equal(run('historySamples.at(-1).fwd'), 0, 'A measured zero is a real value');
    assert.equal(run('historySamples.at(-1).swr'), null, 'Unknown SWR is a gap');
    assert.equal(run('historySamples.at(-1).antenna'), null, 'An unconfirmed antenna is not the last selection');
    assert.equal(run('historyLabel(historySamples.at(-1), "band")'), '20m (last)');
    run('historyVisible = true; drawHistory(); setLanguage("ru")');
    await run('ensureLocale()'); run('drawHistory()');
    assert.equal(ui.element('historyFwd').textContent, 'FWD: 0.0 Вт');
    assert.equal(ui.element('historySwr').textContent, 'КСВ: --');
    assert.equal(ui.element('historyBand').textContent, 'Диапазон: 20m (последний)');
    assert.equal(requests.length, 0, 'Recording, rendering and language changes send no requests');
    run('setHistoryWindow(60); setHistoryWindow("bad")');
    assert.equal(run('historyWindow'), 60000);
    assert.equal(run('historySamples.length'), 2, 'Window changes preserve collected history');
    ui.socket.close();
    assert.equal(run('historySamples.length'), 3, 'Disconnect adds a gap without losing earlier history');
    assert.equal(run('historySamples.at(-1).fwd'), null);
    assert.equal(run('historySamples.at(-1).mode'), null);

    // A late HTTP status must never append an old snapshot after a newer WS frame.
    ui.socket.open();
    server.telemetry = { ...snapshot, fwd_power: 12 };
    server.holdStatus = deferred();
    const pending = run('refreshMqttStatus()');
    await tick();
    ui.socket.message({ ...snapshot, fwd_power: 99 });
    const count = run('historySamples.length');
    server.holdStatus.resolve();
    await pending;
    delete server.holdStatus;
    assert.equal(run('historySamples.length'), count);
    assert.equal(run('historySamples.at(-1).fwd'), 99);
    run('wsLastMeterAt = null');
    await run('refreshMqttStatus()');
    assert.equal(run('historySamples.at(-1).fwd'), 12, 'Accepted HTTP fallback also feeds history');

    // Real 20 Hz input must retain the entire selected 15/30 minute window.
    run(`clearHistory();
        var savedNow = Date.now;
        for (let i = 0; i <= 36000; i++) {
            Date.now = () => 200000 + i * 50;
            appendHistory({ fwd_power: i % 200, ref_power: i % 10, swr: 1 + i % 5, band: '80m', mode: 4, antenna: 1 });
        }
        Date.now = savedNow;`);
    assert.equal(run('historySamples.at(-1).at - historySamples[0].at'), 1800000);
    assert.ok(run('historySamples.length <= historyLimit'));
    assert.equal(run('historyAt(historySamples.at(-1).at - 900000).band'), '80m');
    // Burst input is compacted by time bucket, preserving real extrema and endpoints.
    run(`clearHistory();
        var savedNow = Date.now;
        for (let i = 0; i < 1000; i++) {
            Date.now = () => 200000 + i;
            appendHistory({ fwd_power: i === 37 ? 999 : 10, ref_power: i === 49 ? 77 : 0,
                swr: i === 51 ? 9 : i === 60 ? null : 1, band: '80m', mode: 4, antenna: 1 });
        }
        Date.now = savedNow;`);
    assert.ok(run('historySamples.length <= 32'));
    for (const [key, peak] of [['fwd', 999], ['ref', 77], ['swr', 9]])
        assert.ok(run(`historySamples.some(sample => sample.${key} === ${peak})`), 'Preserve peak ' + key);
    assert.ok(run('historySamples.some(sample => sample.breaks?.swr)'), 'Compaction preserves invalid-SWR gaps');
    run('pruneHistory(historySamples.at(-1).at + historyRetention + 1)');
    assert.equal(run('historySamples.length'), 0);
    run(`appendHistory({ fwd_power: -1, ref_power: Infinity, swr: 0, mode: 99, antenna: 3, band: '<img>' })`);
    for (const field of ['fwd', 'ref', 'swr', 'mode', 'antenna', 'band'])
        assert.equal(run(`historySamples[0].${field}`), null, 'Invalid field: ' + field);
    run(`clearHistory(); appendHistory({ fwd_power: 0, band: '160m', radio: {
        provider: 'tci', state: 'ready', age_ms: 20, band: '80m', tx_frequency_hz: 3650000, rx_tx: 'rx', tuning: false } });`);
    assert.equal(run('historySamples.at(-1).tciBand'), '80m', 'TCI band is independent of LDG decoding');
    run(`appendHistory({ fwd_power: 0, radio: { provider: 'tci', state: 'disconnected', age_ms: 20,
        band: '80m', tx_frequency_hz: 3650000 } });`);
    assert.equal(run('historySamples.at(-1).tciBand'), null, 'Disconnected TCI never holds a band');
    run(`setHistoryTxOnly(true);
        var savedNow = Date.now;
        var radio = { provider: 'tci', state: 'ready', age_ms: 10, band: '80m', tx_frequency_hz: 3650000, rx_tx: 'tx', tuning: false };
        for (let cycle = 0; cycle < 20; cycle++) {
            for (let sec = 0; sec <= 50; sec++) {
                Date.now = () => 300000 + cycle * 170000 + sec * 1000;
                appendHistory({ fwd_power: sec === 50 ? 0 : 40, swr: 1, band: '160m',
                    radio: { ...radio, rx_tx: sec === 50 ? 'rx' : 'tx' } });
            }
        }
        Date.now = savedNow;`);
    assert.equal(run('historyElapsed'), 1000000, 'Only 20 x 50 seconds TX, with two-minute RX pauses');
    assert.ok(run('historyAt(historyElapsed - 900000) !== null'), 'Full 15 minutes of cumulative TX');
    assert.ok(run('historySamples.every(sample => sample.fwd > 0)'), 'No RX zeros recorded');
    run('drawHistory()');
    assert.match(ui.element('historyTime').textContent, /16:40 TX/);
    assert.equal(run('historyAt(historyElapsed + 1)'), null);
    const elapsed = run('historyElapsed');
    run(`var savedNow = Date.now; Date.now = () => historyPrevious.wall + 120000;
        appendHistory({ fwd_power: 99, radio: { provider: 'tci', state: 'ready', age_ms: 0, rx_tx: 'rx', tuning: false } });
        Date.now = savedNow;`);
    assert.equal(run('historyElapsed'), elapsed, 'TCI RX excludes spurious RF and freezes time');
    run(`clearHistory();
        var savedNow = Date.now;
        var radio = { provider: 'tci', state: 'ready', age_ms: 0, rx_tx: 'rx', tuning: true };
        Date.now = () => 10000; appendHistory({ fwd_power: 0, radio });
        Date.now = () => 11000; appendHistory({ fwd_power: 0, radio });
        Date.now = () => 20000; appendHistory({ fwd_power: 0, radio });
        Date.now = savedNow;`);
    assert.equal(run('historyElapsed'), 1000, 'TUNE counts TX even without TRX; outages do not count as TX');
    assert.notEqual(run('historySamples[0].segment'), run('historySamples.at(-1).segment'));
    run(`clearHistory(); var savedNow = Date.now;
        Date.now = () => 10000; appendHistory({ fwd_power: 40 });
        Date.now = () => 11000; appendHistory({ fwd_power: 40 });
        Date.now = () => 12000; appendHistory({ fwd_power: 0 });
        Date.now = () => 112000; appendHistory({ fwd_power: 0 });
        Date.now = savedNow;`);
    assert.equal(run('historyElapsed'), 2000, 'Without TCI, RF controls recording');
    run('setHistoryTxOnly(false)');
    assert.equal(run('historySamples.length'), 0, 'Changing time bases starts a fresh history');
    run('clearHistory(); drawHistory()');
    assert.equal(ui.element('historyTime').textContent, 'Ожидание данных для графиков…');

    const reopened = await page(board());
    assert.equal(vm.runInContext('historySamples.length', reopened.context), 0, 'A new page has no prior history');
    console.log('PASS: browser-only history, confirmed states, zeros/nulls, gaps, RU/EN, no requests, WS/HTTP ordering, retention and clear');
})().catch(error => { console.error(error); process.exitCode = 1; });
