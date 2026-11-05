// Run the production page script with controllable command completions.
const assert = require('node:assert/strict');
const vm = require('node:vm');
const { board, page, tick, deferred } = require('./test_meter_scale_web.cjs');

(async () => {
    const server = board();
    const fetch = server.fetch;
    const commands = [];
    server.fetch = async function(url, options) {
        if (!url.startsWith('/api/command/')) return fetch.call(this, url, options);
        assert.equal(options.method, 'POST');
        commands.push(url);
        this.lastCommandSignal = options.signal;
        if (this.holdCommand) await Promise.race([
            this.holdCommand.promise,
            new Promise((_, reject) => options.signal.addEventListener('abort', () => reject(new Error('aborted')), { once: true }))
        ]);
        if (this.failCommand) throw new Error('offline');
        return { ok: true, json: async () => ({ result: 'success' }) };
    };
    const ui = await page(server);
    ui.socket.message({ antenna: 0, antenna_last: 2, antenna_confirmed: false, antenna_init: 'ready' });
    assert.match(ui.element('btnAnt2').className, /unconfirmed/);
    assert.doesNotMatch(ui.element('btnAnt2').className, /\bactive\b/);
    assert.match(ui.element('antennaInitStatus').textContent, /последняя ANT2/);
    ui.socket.message({ antenna: 1, antenna_last: 1, antenna_confirmed: true, antenna_init: 'ready' });
    assert.match(ui.element('btnAnt1').className, /\bactive\b/);
    assert.equal(ui.element('antennaInitStatus').textContent, 'Готово');
    ui.socket.message({ antenna: 0, antenna_last: 0 });
    server.holdCommand = deferred();
    const first = vm.runInContext('selectAntenna(1)', ui.context);
    await tick();
    vm.runInContext('selectAntenna(1); selectAntenna(2);', ui.context);
    assert.deepEqual(commands, ['/api/command/antenna_1']);
    server.holdCommand.resolve();
    await first;
    delete server.holdCommand;
    ui.socket.message({ ...server.snapshot(), fwd_power: 100, ref_power: 1, swr: 1.2, antenna: 1 });
    await vm.runInContext('selectAntenna(1)', ui.context);
    assert.equal(commands.length, 1);
    await vm.runInContext('selectAntenna(2)', ui.context);
    assert.equal(commands.at(-1), '/api/command/antenna_2');
    ui.socket.message({ ...server.snapshot(), mode: 1, fwd_power: 100, ref_power: 1, swr: 1.2 });
    assert.equal(ui.element('btnMode').textContent, 'Set Manual');
    await vm.runInContext('toggleTuningMode()', ui.context);
    assert.equal(commands.at(-1), '/api/command/manual');
    ui.socket.message({ ...server.snapshot(), mode: 2, fwd_power: 100, ref_power: 1, swr: 1.2 });
    assert.equal(ui.element('btnMode').textContent, 'Set Auto');
    await vm.runInContext('toggleTuningMode()', ui.context);
    assert.equal(commands.at(-1), '/api/command/auto');

    assert.equal(ui.element('btnBypass').textContent, 'BYPASS ON');
    await vm.runInContext('toggleBypass()', ui.context);
    assert.equal(commands.at(-1), '/api/command/bypass');
    assert.equal(ui.element('btnBypass').textContent, 'BYPASS ON', 'Wait for confirmed bypass state');
    ui.socket.message({ ...server.snapshot(), mode: 3 });
    assert.equal(ui.element('btnBypass').textContent, 'BYPASS OFF');
    server.failCommand = true;
    await vm.runInContext('toggleBypass()', ui.context);
    assert.equal(commands.at(-1), '/api/command/auto');
    assert.equal(ui.element('btnBypass').textContent, 'BYPASS OFF', 'A failed exit must not clear bypass');
    delete server.failCommand;
    await vm.runInContext('toggleBypass()', ui.context);
    assert.equal(commands.at(-1), '/api/command/auto');
    assert.equal(ui.element('btnBypass').textContent, 'BYPASS OFF', 'Wait for confirmed AUTO state');
    ui.socket.message({ ...server.snapshot(), mode: 1 });
    assert.equal(ui.element('btnBypass').textContent, 'BYPASS ON');

    server.holdCommand = deferred();
    const tune = vm.runInContext("sendCommand('full_tune')", ui.context);
    await tick();
    await ui.advance(22000);
    assert.equal(server.lastCommandSignal.aborted, false, 'Remote full tune must outlive the UART cycle');
    assert.equal(vm.runInContext('commandPending', ui.context), true);
    assert.match(ui.element('toast').textContent, /Waiting for tuner/);
    assert.match(ui.element('toast').className, /pending/);
    server.holdCommand.resolve();
    await tune;
    assert.match(ui.element('toast').textContent, /completed/);
    await ui.advance(10000);
    assert.equal(server.lastCommandSignal.aborted, false, 'Completed requests must clear their abort timer');

    server.holdCommand = deferred();
    const stalled = vm.runInContext("sendCommand('full_tune')", ui.context);
    await tick();
    await ui.advance(30000);
    await stalled;
    assert.equal(server.lastCommandSignal.aborted, true);
    assert.equal(vm.runInContext('commandPending', ui.context), false);
    assert.match(ui.element('toast').textContent, /result unavailable/);
    delete server.holdCommand;
    server.failCommand = true;
    await vm.runInContext("sendCommand('full_tune')", ui.context);
    assert.equal(vm.runInContext('commandPending', ui.context), false);
    assert.match(ui.element('toast').textContent, /result unavailable/);

    vm.runInContext(`
        globalThis.trigCalls = 0;
        for (const name of ['sin', 'cos']) {
            const original = Math[name];
            Math[name] = (...args) => { trigCalls++; return original(...args); };
        }
        drawMeter();
    `, ui.context);
    assert.equal(vm.runInContext('trigCalls', ui.context), 0, 'Unchanged meter must not redraw');
    ui.socket.message({ ...server.snapshot(), fwd_power: 200, ref_power: 1, swr: 1.2 });
    assert.equal(vm.runInContext('trigCalls', ui.context), 4, 'Changed needles must reuse cached scale');
    vm.runInContext('trigCalls = 0', ui.context);
    ui.socket.message({ meter_scale: 100, meter_scale_revision: 1, fwd_power: 200, ref_power: 1, swr: 1.2 });
    assert.equal(vm.runInContext('trigCalls', ui.context), 2040, 'New scale must rebuild its face once');
    const labels = [];
    ui.element('meterCanvas').getContext().fillText = text => labels.push(text);
    ui.socket.message({ fwd_power: 0, ref_power: 17.74, swr: null });
    assert.equal(vm.runInContext('meterState.swr', ui.context), null);
    assert.ok(labels.includes('SWR --'), 'Invalid RF must not be displayed as a good match');
    ui.socket.close();
    assert.equal(vm.runInContext('meterState.swr', ui.context), null);
    assert.equal(vm.runInContext('activeAntenna', ui.context), 0);
    console.log('PASS: antenna commands, duplicate taps, long tuning, bounded timeout, failed requests, cache reuse and disconnect invalidation');
})().catch(error => { console.error(error); process.exitCode = 1; });
