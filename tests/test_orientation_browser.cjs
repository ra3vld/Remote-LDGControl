// NODE_PATH=<Playwright modules> CHROMIUM_EXECUTABLE=/usr/bin/chromium node tests/test_orientation_browser.cjs
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { chromium } = require('playwright');
const { html, serveAsset } = require('./web_assets.cjs');

(async () => {
    const browser = await chromium.launch({ headless: true,
        executablePath: process.env.CHROMIUM_EXECUTABLE || undefined, args: ['--no-sandbox'] });
    try {
        const context = await browser.newContext({ locale: 'ru-RU', viewport: { width: 375, height: 900 } });
        let orientation = 0, failedSave = false, headless = false;
        const posts = [], errors = [];
        const snapshot = { fwd_power: 0, ref_power: 0, swr: null, mode: 1, antenna: 1, band: '20m',
            meter_scale: 1000, meter_logarithmic: false, meter_scale_revision: 0 };
        await context.addInitScript(snapshot => {
            window.WebSocket = class { constructor() {
                setTimeout(() => { this.onopen(); this.onmessage({ data: JSON.stringify(snapshot) }); }, 0);
            } };
        }, snapshot);
        await context.route('http://ldg.test/**', async route => {
            if (await serveAsset(route)) return;
            const request = route.request(), pathname = new URL(request.url()).pathname;
            if (pathname === '/') return route.fulfill({ contentType: 'text/html', body: html });
            if (request.method() === 'POST') {
                assert.equal(pathname, '/api/config');
                const value = request.postDataJSON(); posts.push(value);
                assert.deepEqual(Object.keys(value), ['display_orientation']);
                if (failedSave) return route.fulfill({ status: 500, json: { result: 'error' } });
                orientation = value.display_orientation;
                return route.fulfill({ json: { result: 'ok', restart: false } });
            }
            const replies = {
                '/api/ws-token': { token: 'test' },
                '/api/config': headless ? { build: 'remote' } : { build: 'display', display_orientation: orientation, mqttBroker: 'Broker' },
                '/api/antenna-config': { enabled: false, bands: Array(11).fill(0) },
                '/api/radio-config': { enabled: false, host: '', port: 40001, receiver: 0 },
                '/api/tuning-config': { auto_fallback: false, swr_threshold: 2 },
                '/api/display-config': { supported: false },
                '/api/status': { ...snapshot, mqtt: { state: 'disabled', enabled: false, message: 'Disabled' } }
            };
            assert.ok(replies[pathname], 'Unexpected request: ' + pathname);
            return route.fulfill({ json: replies[pathname] });
        });
        const page = await context.newPage(); page.on('pageerror', error => errors.push(error.message));
        await page.goto('http://ldg.test/');
        await page.evaluate(() => loadAdvancedSettings());
        await page.waitForFunction(() => displayOrientationLoaded);
        await page.locator('[data-tab="settings"]').click();
        await page.locator('#mqttBroker').fill('Draft broker');
        await page.locator('#displayRotation').selectOption('3');
        await page.locator('#displayMirrorX').check();
        await page.locator('#displayMirrorY').check();
        assert.equal(posts.length, 0);
        await page.locator('#languageSelect').selectOption('en');
        assert.equal(await page.locator('#displayRotation').inputValue(), '3');
        assert.equal(await page.locator('#displayMirrorX').isChecked(), true);
        assert.equal(await page.locator('#displayMirrorY').isChecked(), true);
        await page.locator('#btnSaveOrientation').click();
        await page.waitForFunction(() => !displayOrientationPending);
        assert.deepEqual(posts, [{ display_orientation: 15 }]);
        assert.equal(await page.locator('#mqttBroker').inputValue(), 'Draft broker');
        assert.equal(await page.locator('#orientationStatus').textContent(), 'Display orientation saved');
        await page.locator('#languageSelect').selectOption('ru');
        await page.evaluate(() => ensureLocale());
        assert.equal(await page.locator('#orientationStatus').textContent(), 'Ориентация дисплея сохранена');
        for (const width of [320, 375, 1280]) {
            await page.setViewportSize({ width, height: 900 });
            assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth), false,
                'Horizontal overflow at ' + width);
            if (process.env.ORIENTATION_SCREENSHOTS) {
                fs.mkdirSync(process.env.ORIENTATION_SCREENSHOTS, { recursive: true });
                await page.locator('#orientationControls').scrollIntoViewIfNeeded();
                await page.screenshot({ path: path.join(process.env.ORIENTATION_SCREENSHOTS, 'settings-' + width + '.png') });
            }
        }
        await page.reload();
        await page.evaluate(() => loadAdvancedSettings());
        await page.waitForFunction(() => displayOrientationLoaded);
        await page.locator('[data-tab="settings"]').click();
        assert.equal(await page.locator('#displayRotation').inputValue(), '3');
        assert.equal(await page.locator('#displayMirrorX').isChecked(), true);
        assert.equal(await page.locator('#displayMirrorY').isChecked(), true);
        failedSave = true;
        await page.locator('#displayRotation').selectOption('1');
        await page.locator('#btnSaveOrientation').click();
        await page.waitForFunction(() => !displayOrientationPending);
        assert.equal(orientation, 15);
        assert.equal(await page.locator('#displayRotation').inputValue(), '1');
        assert.equal(await page.locator('#toast').textContent(), 'Не удалось сохранить ориентацию дисплея');
        headless = true;
        await page.reload();
        await page.evaluate(() => loadAdvancedSettings());
        await page.locator('[data-tab="settings"]').click();
        await page.waitForFunction(() => document.getElementById('orientationStatus').textContent.includes('недоступна'));
        assert.equal(await page.locator('#btnSaveOrientation').isDisabled(), true);
        assert.deepEqual(errors, []);
        await context.close();
        console.log('PASS: Chromium rotation/mirrors, partial save, draft preservation, RU/EN, reload, errors, disabled headless controls and 320/375/1280px');
    } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
