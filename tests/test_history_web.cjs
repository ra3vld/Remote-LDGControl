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

    // Bound both elapsed time and number of points, including unusually fast input.
    run(`clearHistory();
        const savedNow = Date.now;
        for (let i = 0; i < historyLimit + 50; i++) {
            Date.now = () => 200000 + i;
            appendHistory({ fwd_power: i, band: '20m', mode: 1, antenna: 1 });
        }
        Date.now = savedNow;`);
    assert.equal(run('historySamples.length'), run('historyLimit'));
    assert.equal(run('historySamples[0].fwd'), 50);
    run('pruneHistory(historySamples.at(-1).at + historyRetention + 1)');
    assert.equal(run('historySamples.length'), 0);
    run(`appendHistory({ fwd_power: -1, ref_power: Infinity, swr: 0, mode: 99, antenna: 3, band: '<img>' })`);
    for (const field of ['fwd', 'ref', 'swr', 'mode', 'antenna', 'band'])
        assert.equal(run(`historySamples[0].${field}`), null, 'Invalid field: ' + field);
    run('clearHistory(); drawHistory()');
    assert.equal(ui.element('historyTime').textContent, 'Ожидание данных для графиков…');

    const reopened = await page(board());
    assert.equal(vm.runInContext('historySamples.length', reopened.context), 0, 'A new page has no prior history');
    console.log('PASS: browser-only history, confirmed states, zeros/nulls, gaps, RU/EN, no requests, WS/HTTP ordering, retention and clear');
})().catch(error => { console.error(error); process.exitCode = 1; });
