// Run with Playwright available via NODE_PATH (or installed locally):
// CHROMIUM_EXECUTABLE=/usr/bin/chromium node tests/test_language_browser.cjs
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { chromium } = require('playwright');

const { html, serveAsset } = require('./web_assets.cjs');
const meter = { meter_scale: 1000, meter_logarithmic: false, meter_scale_revision: 0,
    fwd_power: 40, ref_power: 1, swr: 1.4, band: '20m', mode: 1, tuner_mode: 1,
    antenna: 1, antenna_init: 'ready', antenna_auto: 'selected', antenna_auto_band: '20m',
    tuning_state: 'matched', radio: { state: 'ready', rx_tx: 'rx', tx_frequency_hz: 14200000, band: '20m' } };
const brightness = { supported: true, active: true, brightness: 75, light_raw: 1500,
    automatic: false, manual: 75, minimum: 15, maximum: 100, dark_adc: 3500, bright_adc: 300 };

(async () => {
    const browser = await chromium.launch({ headless: true,
        executablePath: process.env.CHROMIUM_EXECUTABLE || undefined, args: ['--no-sandbox'] });
    try {
        async function session(locale, blockedStorage = false) {
            const context = await browser.newContext({ locale, viewport: { width: 375, height: 850 } });
            await context.addInitScript(({ meter, blockedStorage }) => {
                if (blockedStorage) Object.defineProperty(window, 'localStorage', { get() { throw new Error('blocked'); } });
                window.WebSocket = class {
                    constructor() {
                        window.testSocket = this;
                        setTimeout(() => { this.onopen(); this.onmessage({ data: JSON.stringify(meter) }); }, 0);
                    }
                };
            }, { meter, blockedStorage });
            const requests = [], errors = [];
            let failSave = false;
            await context.route('http://ldg.test/**', async route => {
            if (await serveAsset(route)) return;
                const request = route.request(), url = new URL(request.url());
                if (url.pathname === '/') return route.fulfill({ contentType: 'text/html', body: html });
                requests.push({ method: request.method(), url: url.pathname, body: request.postData() });
                if (request.method() === 'POST') return route.fulfill({ json: failSave
                    ? { result: 'error' } : { result: 'ok', restart: false } });
                const replies = {
                    '/api/ws-token': { token: 'test' },
                    '/api/config': { ant1Name: 'Connected', ant2Name: '<script>', mqttBroker: 'Connected',
                        wifiSSID: 'Settings', mqttPort: 1883, webUsername: 'admin', meterPsuVoltage: 13.8 },
                    '/api/antenna-config': { enabled: true, bands: Array(11).fill(0) },
                    '/api/radio-config': { enabled: false, host: '', port: 40001, receiver: 0 },
                    '/api/tuning-config': { auto_fallback: false, swr_threshold: 2 },
                    '/api/display-config': brightness,
                    '/api/status': { ...meter, display: brightness, uptime_ms: 123000, firmware_build: 'test-build',
                        mqtt: { state: 'connected', enabled: true, broker: 'Connected', port: 1883,
                            attempts: 1, message: 'Connected', connectedForMs: 15000 } }
                };
                assert.ok(replies[url.pathname], 'Unexpected request: ' + url.pathname);
                await route.fulfill({ json: replies[url.pathname] });
            });
            const page = await context.newPage();
            page.on('pageerror', error => errors.push(error.message));
            await page.goto('http://ldg.test/');
        await page.evaluate(() => loadAdvancedSettings());
            await page.waitForFunction(() => meterSettingsLoaded && brightnessLoaded && !mqttStatusPending &&
                document.getElementById('deviceRuntime').textContent.includes('test-build'));
            return { context, page, requests, errors, failSave() { failSave = true; } };
        }

        const en = await session('en-US');
        const { page } = en;
        assert.equal(await page.locator('html').getAttribute('lang'), 'en');
        assert.equal(await page.locator('[data-tab="settings"]').textContent(), 'Settings');
        await page.locator('[data-tab="settings"]').click();
        await page.locator('#mqttBroker').fill('Draft broker');
        await page.locator('#meterRange').fill('42');
        await page.locator('#brightnessManual').fill('33');
        await page.locator('#antennaBand20m').selectOption('2');
        const english = await page.locator('[data-i18n]').allTextContents();
        assert.ok(english.every(text => !/[А-Яа-яЁё]/.test(text)), 'English UI contains Russian text');
        const requestCount = en.requests.length;
        await page.locator('#languageSelect').selectOption('ru');
        await page.evaluate(() => ensureLocale());
        assert.equal(en.requests.length, requestCount, 'Language selection must not call the board');
        assert.equal(await page.locator('html').getAttribute('lang'), 'ru');
        assert.equal(await page.locator('[data-tab="settings"]').textContent(), 'Настройки');
        assert.equal(await page.locator('#mqttStatusMessage').textContent(), 'Подключено');
        assert.match(await page.locator('#brightnessStatus').textContent(), /Яркость: 75%/);
        assert.equal(await page.locator('#antennaInitStatus').textContent(), 'Готово');
        assert.equal(await page.locator('#tuningStatus').textContent(), 'MEM TUNE завершён');
        assert.match(await page.locator('#radioStatus').textContent(), /TCI подключён/);
        assert.equal(await page.locator('#mqttPass').getAttribute('placeholder'), 'Оставьте пустым, чтобы сохранить пароль');
        assert.equal(await page.locator('#ant1Label').textContent(), 'Connected', 'User names must remain verbatim');
        assert.equal(await page.locator('#ant2Label').textContent(), '<script>', 'User names must be safe text');
        assert.equal(await page.locator('#ssid').inputValue(), 'Settings');
        for (const [id, value] of [['mqttBroker', 'Draft broker'], ['meterRange', '42'],
            ['brightnessManual', '33'], ['antennaBand20m', '2']])
            assert.equal(await page.locator('#' + id).inputValue(), value, 'Language switch must preserve drafts');
        for (const width of [320, 375, 1280]) {
            await page.setViewportSize({ width, height: 850 });
            assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth), false,
                'Horizontal overflow at ' + width);
            if (process.env.LANGUAGE_SCREENSHOTS && width !== 320) {
                fs.mkdirSync(process.env.LANGUAGE_SCREENSHOTS, { recursive: true });
                await page.screenshot({ path: path.join(process.env.LANGUAGE_SCREENSHOTS, 'settings-ru-' + width + '.png'), fullPage: true });
                await page.locator('[data-tab="control"]').click();
                await page.screenshot({ path: path.join(process.env.LANGUAGE_SCREENSHOTS, 'control-ru-' + width + '.png'), fullPage: true });
                await page.locator('[data-tab="settings"]').click();
            }
        }
        await page.locator('#languageSelect').selectOption('en');
        assert.deepEqual(await page.locator('[data-i18n]').allTextContents(), english, 'EN/RU round trip');

        // A language switch must immediately translate the latest error snapshot
        // and leave broker/topic strings intact, even when they resemble labels.
        await page.evaluate(() => updateMqttStatus({ state: 'disconnected', enabled: true,
            broker: '<b>Connected</b>', port: 1883, attempts: 3, retryInMs: 2000,
            message: 'Disconnected; waiting to reconnect', errorActive: true,
            lastError: { message: 'Connection failed', code: -2, ageMs: 3000,
                detail: 'TCP connection failed (check broker address, DNS, port, and network)', topic: '<img src=x>' } }));
        await page.locator('#languageSelect').selectOption('ru');
        await page.evaluate(() => ensureLocale());
        assert.match(await page.locator('#mqttStatusError').textContent(), /Ошибка подключения.*код -2.*<img src=x>/);
        assert.match(await page.locator('#mqttStatusDetail').textContent(), /<b>Connected<\/b>.*Повтор через 2 с/);
        assert.equal(await page.locator('#mqttStatusDetail b, #mqttStatusError img').count(), 0);
        assert.match(await page.locator('#mqttIndicator').getAttribute('title'), /Ошибка подключения.*\nОткрыть настройки MQTT/s);
        await page.evaluate(() => showToast('Settings saved'));
        assert.equal(await page.locator('#toast').textContent(), 'Настройки сохранены');
        await page.locator('#languageSelect').selectOption('en');
        assert.equal(await page.locator('#toast').textContent(), 'Settings saved');

        en.failSave();
        await page.evaluate(() => saveAntennaSettings());
        assert.equal(await page.locator('#toast').textContent(), 'Cannot save antenna table');
        await page.locator('#languageSelect').selectOption('ru');
        await page.evaluate(() => ensureLocale());
        assert.equal(await page.locator('#toast').textContent(), 'Не удалось сохранить таблицу антенн');
        await page.reload();
        assert.equal(await page.locator('html').getAttribute('lang'), 'ru', 'Persist language on reload');
        await page.locator('#languageSelect').selectOption('en');
        await page.reload();
        assert.equal(await page.locator('html').getAttribute('lang'), 'en');
        assert.deepEqual(en.errors, []);
        await en.context.close();

        for (const [locale, blocked, expected] of [['ru-RU', false, 'ru'], ['ru-RU', true, 'ru'], ['de-DE', true, 'en']]) {
            const test = await session(locale, blocked);
            assert.equal(await test.page.locator('html').getAttribute('lang'), expected);
            await test.page.locator('#languageSelect').selectOption(expected === 'ru' ? 'en' : 'ru');
            assert.equal(await test.page.locator('html').getAttribute('lang'), expected === 'ru' ? 'en' : 'ru');
            assert.deepEqual(test.errors, []);
            await test.context.close();
        }
        console.log('PASS: Chromium RU/EN, live statuses/errors, safe text, drafts, 320/375/1280px, persistence, blocked storage, zero language requests');
    } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
