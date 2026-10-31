const assert = require('node:assert/strict');
const vm = require('node:vm');
const { board, page, tick, deferred } = require('./test_meter_scale_web.cjs');

(async () => {
    const server = board();
    const original = server.fetch;
    const writes = [];
    let statusReads = 0;
    server.fetch = async function(url, options = {}) {
        if (url === '/api/status') {
            statusReads++;
            if (this.holdStatus) await this.holdStatus.promise;
            return { ok: true, json: async () => ({ ...this.snapshot(), uptime_ms: 123456,
                firmware_build: 'test-build', mqtt: { state: 'connected', enabled: true,
                    broker: 'broker', port: 1883, attempts: 1, message: 'Connected' } }) };
        }
        if (url === '/api/config' && options.method === 'POST') {
            writes.push({ url, body: JSON.parse(options.body) });
            return { ok: true, json: async () => ({ result: 'ok', restart: this.networkChange || false }) };
        }
        if (url === '/api/mqtt/reconnect') {
            assert.equal(options.method, 'POST');
            writes.push({ url });
            if (this.holdReconnect) await this.holdReconnect.promise;
            return { ok: true, json: async () => ({ result: 'ok' }) };
        }
        return original.call(this, url, options);
    };
    const ui = await page(server);
    for (const [id, value] of Object.entries({ mqttBroker: 'broker', mqttPort: '1883',
        mqttUser: 'user', mqttPass: '', webUser: 'admin', webPass: '', psuVoltage: '13.8',
        ant1Name: 'Dipole', ant2Name: 'Vertical', staticIP: '192.0.2.10',
        staticNetmask: '255.255.255.0', staticGateway: '192.0.2.1', staticDNS: '', remoteHost: '' }))
        ui.element(id).value = value;
    ui.element('useStaticIP').checked = false;
    ui.element('mqttDiscoveryEnabled').checked = true;
    await vm.runInContext('saveSettings()', ui.context);
    assert.equal(writes.length, 1);
    assert.equal(writes[0].url, '/api/config');
    assert.equal(ui.element('toast').textContent, 'Settings saved');
    assert.doesNotMatch(ui.element('toast').textContent, /restart/i);
    assert.match(ui.element('deviceRuntime').textContent, /123 s.*test-build/);

    server.holdReconnect = deferred();
    const reconnect = vm.runInContext('reconnectMqtt()', ui.context);
    await tick();
    await vm.runInContext('reconnectMqtt()', ui.context);
    assert.equal(writes.filter(item => item.url === '/api/mqtt/reconnect').length, 1);
    assert.equal(ui.element('btnMqttReconnect').disabled, true);
    server.holdReconnect.resolve();
    await reconnect;
    assert.equal(ui.element('btnMqttReconnect').disabled, false);
    assert.equal(writes.filter(item => item.url === '/api/config').length, 1, 'Reconnect must not save config or reboot');
    delete server.holdReconnect;

    server.holdStatus = deferred();
    const before = statusReads;
    const refresh = vm.runInContext('refreshMqttStatus()', ui.context);
    await tick();
    await vm.runInContext('refreshMqttStatus(); refreshMqttStatus();', ui.context);
    assert.equal(statusReads, before + 1, 'Manual refresh must not create concurrent polling loops');
    server.holdStatus.resolve();
    await refresh;
    delete server.holdStatus;
    await ui.advance(3000);
    assert.equal(statusReads, before + 2, 'One periodic poll follows manual refresh');

    server.networkChange = true;
    await vm.runInContext('saveSettings()', ui.context);
    assert.match(ui.element('toast').textContent, /Restarting/);
    console.log('PASS: runtime save, explicit MQTT reconnect, duplicate clicks, single status polling loop and uptime diagnostics');
})().catch(error => { console.error(error); process.exitCode = 1; });
