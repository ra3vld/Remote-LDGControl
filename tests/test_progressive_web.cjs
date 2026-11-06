const assert = require('node:assert/strict');
const vm = require('node:vm');
const { board, page, tick, deferred } = require('./test_meter_scale_web.cjs');
(async () => {
    const server = board(), original = server.fetch.bind(server), requests = [];
    const hold = deferred(); let first = true;
    server.fetch = async (url, options = {}) => {
        requests.push(url);
        if (url === '/api/status' && first) {
            first = false;
            await new Promise((resolve, reject) => {
                hold.promise.then(resolve);
                options.signal.addEventListener('abort', () => reject(new Error('aborted')), { once: true });
            });
        }
        if (url.startsWith('/api/command/')) return { ok: true, json: async () => ({ result: 'success' }) };
        return original(url, options);
    };
    const ui = await page(server, true, { optional: false });
    const run = source => vm.runInContext(source, ui.context);
    assert.equal(run('coreReady'), false);
    await run('sendCommand("memory_tune")'); await tick();
    assert.deepEqual(requests.slice(0, 4), ['/api/ws-token', '/api/status', '/api/command/memory_tune', '/api/status']);
    assert.equal(run('coreReady'), true, 'Interrupted background read resumes after the command');
    assert.equal(ui.element('btnSaveTuning').disabled, false);

    const writes = [];
    const configured = board(), transport = configured.fetch.bind(configured);
    configured.fetch = async (url, options = {}) => {
        if (url === '/api/antenna-config' && options.method === 'POST') {
            writes.push(JSON.parse(options.body));
            return { ok: true, json: async () => ({ result: 'ok' }) };
        }
        return transport(url, options);
    };
    const critical = await page(configured, true, { optional: false });
    critical.element('antennaAutoEnabled').checked = true;
    await vm.runInContext('saveAntennaAutomation()', critical.context);
    assert.deepEqual(writes, [{ enabled: true, bands: Array(11).fill(0) }]);
    assert.equal(vm.runInContext('settingsModule', critical.context), null,
        'Critical automation saves without loading the full table editor');

    const scripts = [];
    const slow = await page(board(), true, { optional: false, loadScript: (script, execute) => scripts.push({ script, execute }) });
    const evaluate = source => vm.runInContext(source, slow.context);
    const firstLoad = evaluate('loadVisuals()'); await tick();
    assert.equal(scripts.length, 1);
    await slow.advance(8001); await firstLoad;
    assert.equal(slow.element('retryVisuals').hidden, false);
    const retry = evaluate('loadVisuals()'); await tick();
    assert.equal(scripts.length, 2);
    scripts[0].execute(); // A removed script may still arrive; it cannot install stale code.
    assert.equal(evaluate('visualModule'), null);
    scripts[1].execute(); await retry;
    assert.equal(evaluate('visualModule !== null'), true);
    assert.equal(slow.element('retryVisuals').hidden, true);
    assert.equal(evaluate('historySamples.length'), 0, 'Metadata-only snapshots do not create history');
    console.log('PASS: commands interrupt/resume background reads, bounded module timeout, explicit retry and rejection of late superseded scripts');
})().catch(error => { console.error(error); process.exitCode = 1; });
