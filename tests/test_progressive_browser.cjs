// Exercise the production minified modules through real Chromium script/SRI loading.
const assert = require('node:assert/strict');
const { chromium } = require('playwright');
const { html, asset, report } = require('./web_assets.cjs');
const snapshot = { fwd_power: 42, ref_power: 1, swr: 1.4, band: '20m', mode: 1, antenna: 1,
    meter_scale: 1000, meter_logarithmic: false, meter_scale_revision: 0 };
const tick = () => new Promise(resolve => setTimeout(resolve, 50));
(async () => {
    const browser = await chromium.launch({ headless: true,
        executablePath: process.env.CHROMIUM_EXECUTABLE || undefined, args: ['--no-sandbox'] });
    try {
        async function session(mode = 'hold') {
            const context = await browser.newContext({ locale: 'ru-RU', viewport: { width: 375, height: 900 } });
            const requests = [], errors = [], held = [];
            let failSettings = true;
            await context.addInitScript(({ snapshot, mode }) => {
                window.socketCount = 0;
                window.WebSocket = class { constructor() {
                    window.socketCount++; window.testSocket = this;
                    setTimeout(() => { this.onopen(); this.onmessage({ data: JSON.stringify(snapshot) }); }, 0);
                } };
                if (mode === 'canvas') HTMLCanvasElement.prototype.getContext = () => null;
            }, { snapshot, mode });
            await context.route('http://ldg.test/**', async route => {
                const request = route.request(), url = new URL(request.url());
                const name = url.searchParams.get('asset');
                if (name) {
                    requests.push(name);
                    if (name.startsWith('widgets.') && mode === 'hold') { held.push(route); return; }
                    // SRI rejects a truncated (syntactically valid) module before execution.
                    if (name.startsWith('widgets.') && mode === 'truncated')
                        return route.fulfill({ contentType: 'text/javascript', body: 'window.partialWidgetExecuted=true;' });
                    if (name.startsWith('settings.') && failSettings) return route.abort();
                    return route.fulfill({ contentType: 'text/javascript', body: asset(request.url()) });
                }
                if (url.pathname === '/') return route.fulfill({ contentType: 'text/html', body: html });
                requests.push(url.pathname);
                if (request.method() === 'POST') return route.fulfill({ json: { result: 'success' } });
                const replies = {
                    '/api/ws-token': { token: 'test' }, '/api/status': { ...snapshot, mqtt: { state: 'disabled', enabled: false } },
                    '/api/config': { ant1Name: 'Dipole', build: 'display', display_orientation: 0 },
                    '/api/tuning-config': { auto_fallback: false, swr_threshold: 2 },
                    '/api/antenna-config': { enabled: true, bands: Array(11).fill(1) },
                    '/api/radio-config': { enabled: false, host: '', port: 40001, receiver: 0 },
                    '/api/display-config': { supported: false }
                };
                assert.ok(replies[url.pathname], 'Unexpected request: ' + url.pathname);
                await route.fulfill({ json: replies[url.pathname] });
            });
            const page = await context.newPage(); page.on('pageerror', error => errors.push(error.message));
            await page.goto('http://ldg.test/');
            await page.waitForFunction(() => coreReady);
            return { page, context, requests, held, errors, settingsWork() { failSettings = false; } };
        }
        const test = await session(); const { page } = test;
        await page.waitForFunction(() => loadingScripts.has('widgets'));
        assert.deepEqual(test.requests.filter(x => x.startsWith('/api/') && x !== '/api/ws-token'),
            ['/api/status', '/api/config', '/api/tuning-config', '/api/antenna-config']);
        assert.equal(await page.locator('#btnAnt1').isEnabled(), true);
        assert.equal(await page.locator('#liveFwd').textContent(), 'FWD: 42.0 Вт');
        assert.equal(await page.locator('#ant1Label').textContent(), 'Dipole');
        assert.equal(await page.evaluate(() => historySamples.length > 0 && visualModule === null), true);
        await page.locator('button[onclick="sendCommand(\'memory_tune\')"]').click();
        await page.waitForFunction(() => !commandPending);
        assert.equal(test.requests.filter(x => x === '/api/command/memory_tune').length, 1);
        await page.evaluate(() => testSocket.onmessage({ data: JSON.stringify({ fwd_power: 88, ref_power: 2, swr: 1.7, band: '40m', mode: 4, antenna: 2 }) }));
        assert.equal(await page.locator('#liveFwd').textContent(), 'FWD: 88.0 Вт');
        const count = await page.evaluate(() => historySamples.length);
        assert.ok(count >= 2, 'History begins before visuals arrive');
        assert.equal(test.requests.some(x => x.startsWith('settings.')), false, 'Unopened settings do not load');
        // Release widgets, then fail the independently requested settings module.
        await test.held[0].fulfill({ contentType: 'text/javascript', body: asset('/?asset=' + report.assets.widgets.file) });
        await page.waitForFunction(() => visualModule !== null);
        assert.equal(await page.evaluate(() => historySamples.length), count);
        await page.locator('[data-tab="settings"]').click();
        await page.locator('#advancedSettings summary').click();
        await page.waitForFunction(() => !document.getElementById('retrySettings').hidden);
        assert.equal(await page.locator('#btnSaveTuning').isEnabled(), true);
        assert.equal(await page.locator('#antennaAutoEnabled').isChecked(), true);
        assert.equal(await page.locator('#advancedSettingsBody').evaluate(el => el.disabled), true);
        test.settingsWork(); await page.locator('#retrySettings').click();
        await page.waitForFunction(() => settingsModule !== null && radioSettingsLoaded);
        assert.equal(await page.locator('#advancedSettingsBody').evaluate(el => el.disabled), false);
        assert.equal(await page.evaluate(() => socketCount), 1, 'Retries do not create another transport');
        assert.deepEqual(test.errors, []);
        await test.context.close();

        for (const mode of ['truncated', 'canvas']) {
            const broken = await session(mode), p = broken.page;
            await p.waitForFunction(() => !document.getElementById('retryVisuals').hidden);
            assert.equal(await p.locator('#btnAnt1').isEnabled(), true);
            await p.locator('button[onclick="sendCommand(\'full_tune\')"]').click();
            await p.waitForFunction(() => !commandPending);
            assert.ok(broken.requests.includes('/api/command/full_tune'));
            assert.equal(await p.evaluate(() => window.partialWidgetExecuted), undefined);
            assert.equal(await p.evaluate(() => visualModule), null);
            assert.deepEqual(broken.errors, []);
            await broken.context.close();
        }
        // An incomplete inline core never enables actions from the HTML shell.
        const context = await browser.newContext();
        await context.route('http://ldg.test/**', route => route.fulfill({ contentType: 'text/html', body: html.slice(0, html.indexOf('<script>')) }));
        const shell = await context.newPage(); await shell.goto('http://ldg.test/');
        assert.equal(await shell.locator('#btnAnt1').isDisabled(), true);
        assert.equal(await shell.locator('#languageSelect').isDisabled(), true);
        await context.close();
        console.log('PASS: staged request order, controls/telemetry/history before visuals, lazy settings failure/retry, SRI truncation, unavailable Canvas, incomplete core');
    } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
