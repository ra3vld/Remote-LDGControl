const assert = require('node:assert/strict');
const vm = require('node:vm');
const { board, page, tick, deferred } = require('./test_meter_scale_web.cjs');

(async () => {
    // Change events (including checkbox-only edits) must survive live updates.
    const editingBoard=board(); editingBoard.logarithmic=true;
    const editing=await page(editingBoard);
    editing.element('meterLogarithmic').checked=false;
    if (editing.element('meterLogarithmic').events.change) editing.element('meterLogarithmic').events.change();
    editingBoard.tap();
    assert.equal(editing.element('meterLogarithmic').checked,false,'Live updates must not recheck LOG');
    editing.element('meterRange').value='75';
    if (editing.element('meterRange').events.change) editing.element('meterRange').events.change();
    await vm.runInContext('saveMeterSettings()',editing.context);
    assert.deepEqual(editingBoard.savedMeter,{watts:75,logarithmic:false});
    editing.element('meterRange').value='250'; // Also protect edits before a change event is delivered.
    editingBoard.tap();
    assert.equal(editing.element('meterRange').value,'250');
    await vm.runInContext('saveMeterSettings()',editing.context);
    assert.deepEqual(editingBoard.savedMeter,{watts:250,logarithmic:false},'Changing only watts must save');

    // A slow save must not discard a new edit made while its response is pending.
    editing.element('meterRange').value='300';
    editing.element('meterRange').events.input();
    editingBoard.holdPost=deferred();
    const inFlight=vm.runInContext('saveMeterSettings()',editing.context);
    await tick();
    editing.element('meterRange').value='400';
    editing.element('meterLogarithmic').checked=true;
    editing.element('meterRange').events.input();
    editingBoard.holdPost.resolve(); await inFlight; delete editingBoard.holdPost;
    assert.equal(editing.element('meterRange').value,'400');
    assert.equal(editing.element('meterLogarithmic').checked,true);
    editingBoard.tap();
    assert.equal(editing.element('meterRange').value,'400');
    await vm.runInContext('saveMeterSettings()',editing.context);
    assert.deepEqual(editingBoard.savedMeter,{watts:400,logarithmic:true});

    const fetchEditing=editingBoard.fetch;
    // The page captured the fetch binding at startup: use a fresh page for main-save checks.
    let configWrites=0;
    editingBoard.fetch=async function(url,options={}) {
        if (url==='/api/config' && options.method==='POST') {
            configWrites++;
            return {ok:true,json:async()=>({result:'ok'})};
        }
        return fetchEditing.call(this,url,options);
    };
    const mainSave=await page(editingBoard);
    mainSave.element('meterRange').value='90';
    await vm.runInContext('saveSettings()',mainSave.context);
    assert.deepEqual(editingBoard.savedMeter,{watts:90,logarithmic:true},'Main Save must apply watts without touching LOG');
    mainSave.element('meterLogarithmic').checked=false;
    mainSave.element('meterLogarithmic').events.change();
    await vm.runInContext('saveSettings()',mainSave.context);
    assert.deepEqual(editingBoard.savedMeter,{watts:90,logarithmic:false});
    const afterReload=await page(editingBoard);
    assert.equal(afterReload.element('meterLogarithmic').checked,false);
    assert.equal(afterReload.element('meterRange').value,'90');
    mainSave.element('meterRange').value='0';
    await vm.runInContext('saveSettings()',mainSave.context);
    assert.equal(configWrites,2,'Invalid scale must stop the main save');
    mainSave.element('meterRange').value='99';
    editingBoard.failPost=true;
    await vm.runInContext('saveSettings()',mainSave.context);
    assert.equal(configWrites,2,'Failed scale persistence must stop the main save');
    assert.equal(mainSave.element('meterRange').value,'99');
    assert.match(mainSave.element('toast').textContent,/Cannot save/);

    const server=board(), a=await page(server), b=await page(server);
    a.element('meterRange').value='600';
    a.element('meterLogarithmic').checked=true;
    a.element('meterRange').events.input();
    server.tap(); // A status update must not overwrite a setting being edited.
    assert.equal(a.element('meterRange').value,'600');
    await vm.runInContext('saveMeterSettings()',a.context);
    assert.deepEqual(server.savedMeter,{watts:600,logarithmic:true});
    for (const p of [a,b]) {
        assert.equal(p.watts(),600);
        assert.equal(p.element('scaleValue').textContent,'600W LOG');
        assert.equal(p.element('meterLogarithmic').checked,true);
        for (const power of [0,0.1,1,60,600]) {
            const roundTrip=vm.runInContext(`powerAtFraction(powerFraction(${power}))`,p.context);
            assert.ok(Math.abs(roundTrip-power)<0.000001);
        }
        assert.ok(vm.runInContext('powerFraction(60)',p.context)>0.5);
    }
    const reopened=await page(server);
    assert.equal(reopened.element('meterRange').value,'600');
    assert.equal(reopened.element('meterLogarithmic').checked,true);
    a.element('meterRange').value='1001';
    const posts=server.posts;
    await vm.runInContext('saveMeterSettings()',a.context);
    assert.equal(server.posts,posts);
    a.element('meterRange').value='200';
    a.element('meterRange').events.input();
    server.failPost=true;
    await vm.runInContext('saveMeterSettings()',a.context);
    assert.equal(a.watts(),600);
    assert.equal(a.element('meterRange').value,'200');
    assert.match(a.element('toast').textContent,/Cannot save/);
    delete server.failPost;

    a.element('brightnessAuto').checked=true;
    vm.runInContext('updateBrightnessControls()',a.context);
    assert.equal(a.element('brightnessManual').disabled,true);
    vm.runInContext("calibrateBrightness('brightnessDark')",a.context);
    assert.equal(a.element('brightnessDark').value,'1500');
    a.element('brightnessBright').value='3500'; // Calibration supports either ADC direction.
    a.element('brightnessMin').value='20';
    a.element('brightnessMax').value='85';
    server.holdBrightness=deferred();
    const save=vm.runInContext('saveBrightnessSettings()',a.context);
    await tick();
    await vm.runInContext('saveBrightnessSettings()',a.context);
    server.holdBrightness.resolve(); await save; delete server.holdBrightness;
    assert.equal(server.brightnessPosts,1);
    assert.equal(server.brightness.automatic,true);
    assert.equal(server.brightness.minimum,20);
    assert.equal(server.brightness.maximum,85);
    assert.equal(server.brightness.dark_adc,1500);
    const reloaded=await page(server);
    assert.equal(reloaded.element('brightnessAuto').checked,true);
    assert.equal(reloaded.element('brightnessMin').value,'20');
    a.element('brightnessMin').value='90';
    await vm.runInContext('saveBrightnessSettings()',a.context);
    assert.equal(server.brightnessPosts,1);
    a.element('brightnessMin').value='25';
    server.failBrightness=true;
    await vm.runInContext('saveBrightnessSettings()',a.context);
    assert.equal(server.brightness.minimum,20);
    assert.match(a.element('toast').textContent,/Cannot save brightness/);
    console.log('PASS: custom/log scale sync, inverse mapping, edited fields, persistence failures, brightness calibration, limits and duplicate saves');
})().catch(error => { console.error(error); process.exitCode=1; });
