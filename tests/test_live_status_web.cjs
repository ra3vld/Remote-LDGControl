const assert = require('node:assert/strict');
const vm = require('node:vm');
const { board, page, tick, deferred } = require('./test_meter_scale_web.cjs');

(async () => {
    const server=board(), ui=await page(server);
    server.telemetry={fwd_power:40,ref_power:1,swr:1.4,band:'40m',mode:1,antenna:2};
    await ui.advance(3000);
    assert.equal(vm.runInContext('meterState.fwdPower',ui.context),40);
    assert.equal(ui.element('bandValue').textContent,'Band: 40m');
    assert.equal(ui.element('btnMode').textContent,'Set Controlled');
    assert.match(ui.element('btnAnt2').className,/\bactive\b/);

    ui.socket.message({...server.snapshot(),fwd_power:25,ref_power:0,swr:1,band:'20m',mode:2,antenna:1});
    await vm.runInContext('refreshMqttStatus()',ui.context);
    assert.equal(vm.runInContext('meterState.fwdPower',ui.context),25,'Fresh socket data takes precedence');
    assert.equal(ui.element('btnMode').textContent,'Set Auto');
    assert.match(ui.element('btnAnt1').className,/\bactive\b/);


    await ui.advance(2000);
    server.holdStatus=deferred();
    const pending=vm.runInContext('refreshMqttStatus()',ui.context);
    await tick();
    ui.socket.message({...server.snapshot(),fwd_power:60,ref_power:0,swr:1,band:'80m',mode:3,antenna:2});
    server.holdStatus.resolve(); await pending; delete server.holdStatus;
    assert.equal(vm.runInContext('meterState.fwdPower',ui.context),60,'Delayed HTTP must not overwrite a newer socket frame');
    assert.equal(ui.element('btnBypass').textContent,'BYPASS OFF');
    assert.equal(ui.element('bandValue').textContent,'Band: 80m');

    ui.socket.close();
    server.telemetry={fwd_power:0,ref_power:0,swr:null,band:'40m',mode:1,antenna:1};
    await vm.runInContext('refreshMqttStatus()',ui.context);
    assert.equal(vm.runInContext('meterState.fwdPower',ui.context),0);
    assert.equal(ui.element('btnBypass').textContent,'BYPASS ON');
    assert.match(ui.element('btnAnt1').className,/\bactive\b/);
    ui.socket.open(); ui.socket.close();
    assert.equal(ui.element('bandValue').textContent,'Band: 40m','Repeated socket failures must preserve fresh HTTP data');
    assert.match(ui.element('btnAnt1').className,/\bactive\b/);
    assert.match(ui.element('connectionStatus').innerHTML,/Connected \(HTTP\)/);
    server.failStatus=true;
    await ui.advance(6000);
    await vm.runInContext('refreshMqttStatus()',ui.context);
    assert.equal(vm.runInContext('meterState.fwdPower',ui.context),null,'Expired HTTP data must clear if both transports fail');
    assert.equal(vm.runInContext('activeAntenna',ui.context),0);
    assert.equal(ui.element('bandValue').textContent,'Band: --');
    console.log('PASS: silent/closed WebSocket HTTP fallback, controls and meter updates, stale HTTP ordering');
})().catch(error=>{console.error(error);process.exitCode=1});
