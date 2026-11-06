const assert = require('node:assert/strict');
const vm = require('node:vm');
const { board, page, deferred, tick } = require('./test_meter_scale_web.cjs');

(async () => {
    const server = board(), baseFetch = server.fetch.bind(server);
    let saved = 0, posts = 0, fail = false, hold = null;
    server.fetch = async (url, options = {}) => {
        if (url !== '/api/config') return baseFetch(url, options);
        if (options.method !== 'POST') return { ok: true, json: async () => ({ build: 'display', display_orientation: saved }) };
        posts++;
        const body = JSON.parse(options.body);
        assert.deepEqual(Object.keys(body), ['display_orientation'], 'Orientation save must preserve other form drafts');
        assert.ok(Number.isInteger(body.display_orientation) && body.display_orientation >= 0 && body.display_orientation < 16);
        if (hold) await hold.promise;
        if (fail) return { ok: false, json: async () => ({ result: 'error' }) };
        saved = body.display_orientation;
        return { ok: true, json: async () => ({ result: 'ok', restart: false }) };
    };
    const ui = await page(server);
    const run = code => vm.runInContext(code, ui.context);
    assert.equal(ui.element('orientationControls').disabled, false);
    assert.equal(ui.element('displayRotation').value, '0');
    ui.element('mqttBroker').value = 'Unsaved broker';
    for (let mode = 0; mode < 16; mode++) {
        ui.element('displayRotation').value = String(mode & 3);
        ui.element('displayMirrorX').checked = !!(mode & 4);
        ui.element('displayMirrorY').checked = !!(mode & 8);
        const before = posts;
        assert.equal(run('readDisplayOrientation()'), mode);
        assert.equal(posts, before, 'Changing a control is only a local draft');
        await run('saveDisplayOrientation()');
        assert.equal(saved, mode);
        assert.equal(ui.element('mqttBroker').value, 'Unsaved broker');
    }
    const reopened = await page(server);
    assert.equal(reopened.element('displayRotation').value, '3');
    assert.equal(reopened.element('displayMirrorX').checked, true);
    assert.equal(reopened.element('displayMirrorY').checked, true);
    ui.element('displayRotation').value = '0';
    hold = deferred();
    const saving = run('saveDisplayOrientation()');
    await tick();
    const inFlight = posts;
    await run('saveDisplayOrientation()');
    assert.equal(posts, inFlight, 'Repeated clicks must not enqueue writes');
    // A new draft made during a save remains intact, including language changes.
    ui.element('displayRotation').value = '2';
    run('setLanguage("ru")');
    hold.resolve(); await saving; hold = null;
    assert.equal(ui.element('displayRotation').value, '2');
    assert.equal(ui.element('orientationStatus').textContent, 'Ориентация дисплея сохранена');
    fail = true;
    const beforeFailure = saved;
    await run('saveDisplayOrientation()');
    assert.equal(saved, beforeFailure);
    assert.equal(ui.element('toast').textContent, 'Не удалось сохранить ориентацию дисплея');
    assert.equal(ui.element('displayRotation').value, '2');
    ui.element('displayRotation').value = '90';
    const beforeInvalid = posts;
    await run('saveDisplayOrientation()');
    assert.equal(posts, beforeInvalid);
    for (const config of [{ build: 'remote' }, { build: 'display' }, { build: 'display', display_orientation: 16 }]) {
        const unavailable = board(), fetch = unavailable.fetch.bind(unavailable);
        unavailable.fetch = async (url, options) => url === '/api/config'
            ? { ok: true, json: async () => config } : fetch(url, options);
        const ui = await page(unavailable);
        assert.equal(ui.element('orientationControls').disabled, true);
        await vm.runInContext('saveDisplayOrientation()', ui.context);
    }
    console.log('PASS: all 16 display modes, save/reload, no draft requests, unrelated drafts, duplicate/failed saves, RU/EN and unsupported firmware');
})().catch(error => { console.error(error); process.exitCode = 1; });
