// NODE_PATH=<Playwright modules> CHROMIUM_EXECUTABLE=/usr/bin/chromium node tests/test_history_browser.cjs
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { chromium } = require('playwright');
const { html, serveAsset } = require('./web_assets.cjs');

(async () => {
    const browser = await chromium.launch({ headless: true,
        executablePath: process.env.CHROMIUM_EXECUTABLE || undefined, args: ['--no-sandbox'] });
    try {
        const context = await browser.newContext({ locale: 'ru-RU', hasTouch: true,
            viewport: { width: 1280, height: 1000 } });
        const requests = [], errors = [];
        const snapshot = { fwd_power: 50, ref_power: 2, swr: 1.5, mode: 1, tuner_mode: 1,
            antenna: 1, band: '20m', meter_scale: 1000, meter_logarithmic: false, meter_scale_revision: 0 };
        await context.addInitScript(snapshot => {
            window.WebSocket = class {
                constructor() {
                    window.testSocket = this;
                    setTimeout(() => { this.onopen(); this.onmessage({ data: JSON.stringify(snapshot) }); }, 0);
                }
            };
            window.historyStrokes = [];
            const proto = CanvasRenderingContext2D.prototype;
            for (const method of ['beginPath', 'moveTo', 'lineTo', 'stroke']) {
                const original = proto[method];
                proto[method] = function(...args) {
                    if (this.canvas.id.startsWith('history')) {
                        if (method === 'beginPath') this.testPath = [];
                        if (method === 'moveTo' || method === 'lineTo') this.testPath?.push([method, ...args]);
                        if (method === 'stroke') window.historyStrokes.push({ id: this.canvas.id,
                            color: this.strokeStyle, points: this.testPath?.slice() });
                    }
                    return original.apply(this, args);
                };
            }
        }, snapshot);
        await context.route('http://ldg.test/**', async route => {
            if (await serveAsset(route)) return;
            const pathname = new URL(route.request().url()).pathname;
            if (pathname === '/') return route.fulfill({ contentType: 'text/html', body: html });
            requests.push({ path: pathname, method: route.request().method() });
            const replies = {
                '/api/ws-token': { token: 'test' }, '/api/config': {},
                '/api/antenna-config': { enabled: false, bands: Array(11).fill(0) },
                '/api/radio-config': { enabled: false, host: '', port: 40001, receiver: 0 },
                '/api/tuning-config': { auto_fallback: false, swr_threshold: 2 },
                '/api/display-config': { supported: false },
                '/api/status': { ...snapshot, mqtt: { state: 'disabled', enabled: false, message: 'Disabled' } }
            };
            assert.ok(replies[pathname], 'Unexpected network request: ' + pathname);
            return route.fulfill({ json: replies[pathname] });
        });
        const page = await context.newPage();
        page.on('pageerror', error => errors.push(error.message));
        await page.goto('http://ldg.test/');
        await page.waitForFunction(() => meterSettingsLoaded && !mqttStatusPending && mqttStatusTimer !== null);
        await page.evaluate(async () => { await loadVisuals(); await ensureLocale(); clearTimeout(mqttStatusTimer); clearHistory(); });
        await page.waitForFunction(() => document.getElementById('historyTime').textContent.includes('Ожидание'));
        await page.locator('#historyWindow').selectOption('60');
        const networkCount = requests.length;

        await page.evaluate(() => {
            const end = Date.now();
            for (let i = 0; i < 120; i++) {
                if (i >= 40 && i < 51) continue; // No received frames for six seconds.
                Date.now = () => end - (120 - i) * 500;
                testSocket.onmessage({ data: JSON.stringify({
                    fwd_power: i > 110 ? 0 : 60 + 30 * Math.sin(i / 8),
                    ref_power: i > 110 ? 0 : 2 + Math.sin(i / 8),
                    swr: i > 110 || (i >= 70 && i < 80) ? null : 1.3 + i / 200,
                    mode: i < 35 ? 1 : i < 85 ? 2 : 4, antenna: i < 60 ? 1 : 2,
                    band: i < 60 ? '40m' : '20m', band_held: i > 110,
                    radio: { provider: 'tci', state: 'ready', age_ms: 10, tx_frequency_hz: 3650000, band: '80m', rx_tx: 'tx', tuning: false }
                }) });
            }
            Date.now = () => end;
            historyStrokes.length = 0;
            drawHistory();
        });
        assert.equal(await page.locator('#historyFwd').textContent(), 'FWD: 0.0 Вт');
        assert.equal(await page.locator('#historySwr').textContent(), 'КСВ: --');
        assert.equal(await page.locator('#historyMode').textContent(), 'Режим: CONTROLLED');
        assert.equal(await page.locator('#historyAntenna').textContent(), 'Антенна: ANT 2');
        assert.equal(await page.locator('#historyBand').textContent(), 'Диапазон: 20m (последний)');
        assert.equal(await page.locator('#historyTciBand').textContent(), 'Диапазон TCI: 80m');
        const paths = await page.evaluate(() => historyStrokes.filter(stroke =>
            ['#4ade80', '#fb923c', '#60a5fa'].includes(stroke.color)));
        for (const color of ['#4ade80', '#fb923c', '#60a5fa']) {
            const stroke = paths.find(stroke => stroke.color === color);
            assert.ok(stroke?.points.filter(point => point[0] === 'moveTo').length >= (color === '#60a5fa' ? 3 : 2),
                'Numeric curves must break at missing frames and null measurements: ' + color);
            assert.ok(stroke.points.every(point => point.slice(1).every(Number.isFinite)), 'Finite Canvas coordinates');
        }
        const recorded = await page.evaluate(() => historySamples.length);
        await page.locator('#historyWindow').selectOption('1800');
        assert.equal(await page.evaluate(() => historySamples.length), recorded);
        await page.locator('#historyWindow').selectOption('60');

        for (const width of [320, 375, 1280]) {
            await page.setViewportSize({ width, height: 1000 });
            await page.evaluate(() => { historyCursor = null; drawHistory(); });
            assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth), false,
                'Horizontal overflow at ' + width);
            for (const id of ['historyPower', 'historySWR', 'historyStates']) {
                const canvas = page.locator('#' + id);
                await canvas.scrollIntoViewIfNeeded();
                const bounds = await canvas.boundingBox();
                const x = bounds.x + 76 + (bounds.width - 84) * 0.25, y = bounds.y + 30;
                await page.mouse.move(x, y);
                await page.waitForFunction(() => historyCursor !== null);
                await page.evaluate(() => drawHistory());
                assert.match(await page.locator('#historyAntenna').textContent(), /ANT 1/,
                    'Inspect the earlier confirmed antenna from any graph');
                await page.touchscreen.tap(x, y);
                assert.equal(await page.evaluate(() => historyCursor !== null), true);
            }
            await page.mouse.move(0, 0);
            await page.evaluate(() => { historyCursor = null; drawHistory(); });
            if (process.env.HISTORY_SCREENSHOTS) {
                fs.mkdirSync(process.env.HISTORY_SCREENSHOTS, { recursive: true });
                await page.screenshot({ path: path.join(process.env.HISTORY_SCREENSHOTS, 'charts-ru-' + width + '.png'), fullPage: true });
            }
        }
        await page.locator('#languageSelect').selectOption('en');
        await page.evaluate(() => drawHistory());
        assert.equal(await page.locator('#historyPanel h2').textContent(), 'Live history');
        assert.equal(await page.locator('#historyBand').textContent(), 'Band: 20m (last)');
        assert.equal(await page.evaluate(() => historySamples.length), recorded, 'Language changes preserve history');
        assert.equal(requests.length, networkCount, 'Chart controls, inspection and rendering send no requests');
        assert.equal(requests.filter(request => request.method !== 'GET').length, 0);

        await page.locator('#historyTxOnly').check();
        assert.equal(await page.evaluate(() => historySamples.length), 0);
        await page.evaluate(() => {
            const base = Date.now();
            for (let cycle = 0; cycle < 3; cycle++) for (let sec = 0; sec <= 10; sec++) {
                Date.now = () => base + cycle * 130000 + sec * 1000;
                testSocket.onmessage({ data: JSON.stringify({
                    fwd_power: sec === 10 ? 0 : 40, ref_power: 0, swr: sec === 10 ? null : 1,
                    band: '160m', mode: 4, antenna: 1,
                    radio: { provider: 'tci', state: 'ready', age_ms: 0, rx_tx: 'rx', tuning: sec < 10,
                        band: '80m', tx_frequency_hz: 3650000 }
                }) });
            }
            Date.now = () => base + 500000;
            historyStrokes.length = 0;
            drawHistory();
        });
        assert.equal(await page.evaluate(() => historyElapsed), 30000, 'RX pauses do not advance TX axis');
        assert.match(await page.locator('#historyTime').textContent(), /^00:30 TX/);
        assert.equal(await page.evaluate(() => historySamples.every(point => point.fwd === 40)), true);
        assert.equal(await page.locator('#historyTciBand').textContent(), 'TCI band: 80m');
        const txPath = await page.evaluate(() => historyStrokes.find(stroke => stroke.color === '#4ade80'));
        assert.equal(txPath.points.filter(point => point[0] === 'moveTo').length, 3,
            'Separate transmissions never join into one curve');
        const frozen = await page.evaluate(() => historySamples.length);
        await page.evaluate(() => { drawHistory(); setLanguage('ru'); });
        await page.evaluate(() => drawHistory());
        assert.equal(await page.getByLabel('Писать график только во время передачи').isChecked(), true);
        assert.equal(await page.evaluate(() => historySamples.length), frozen);
        for (const width of [320, 1280]) {
            await page.setViewportSize({ width, height: 1000 });
            await page.evaluate(() => drawHistory());
            assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth), false);
            if (process.env.HISTORY_SCREENSHOTS) await page.screenshot({ path: path.join(process.env.HISTORY_SCREENSHOTS,
                'charts-tx-' + width + '.png'), fullPage: true });
        }
        assert.equal(requests.length, networkCount, 'TX-only controls also send no requests');
        await page.locator('#historyTxOnly').uncheck();
        await page.evaluate(() => { setLanguage('en'); testSocket.onmessage({ data: JSON.stringify({ fwd_power: 40, swr: 1 }) }); });

        // No stale plateau after a transport outage, even without a close event.
        await page.evaluate(() => { const end = Date.now(); Date.now = () => end + 6000; drawHistory(); });
        assert.equal(await page.locator('#historyFwd').textContent(), 'FWD: --');
        assert.match(await page.locator('#historyTime').textContent(), /No data at this time/);
        await page.getByRole('button', { name: 'Clear history', exact: true }).click();
        await page.waitForFunction(() => historySamples.length === 0);
        assert.equal(await page.evaluate(() => Object.keys(localStorage).some(key => key !== 'ldg.language')), false,
            'No history is written to persistent browser storage');
        await page.reload();
        await page.waitForFunction(() => meterSettingsLoaded && !mqttStatusPending && mqttStatusTimer !== null);
        assert.ok(await page.evaluate(() => historySamples.length < 4), 'Reload has only new live samples');
        assert.deepEqual(errors, []);
        await context.close();
        console.log('PASS: Chromium Canvas curves/gaps, state history, pointer/touch, RU/EN, 320/375/1280px, no chart requests/storage, stale values and reload');
    } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
