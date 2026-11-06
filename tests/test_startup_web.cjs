const assert = require('node:assert/strict');
const {board,page,tick,deferred} = require('./test_meter_scale_web.cjs');

(async () => {
    const server=board(), fetch=server.fetch.bind(server), pending=[];
    const paths=['/api/status','/api/config','/api/tuning-config','/api/antenna-config','/api/radio-config','/api/display-config'];
    let active=0, peak=0;
    server.fetch=async (url,options) => {
        if (!paths.includes(url)) return fetch(url,options);
        const gate=deferred(); pending.push({url,gate}); ++active; peak=Math.max(peak,active);
        await gate.promise;
        // A failed settings endpoint must still allow the rest to load.
        const response=url==='/api/antenna-config' ? {ok:false,status:503}
            : url==='/api/radio-config' ? {ok:true,json:async () => ({enabled:false,host:'',port:40001,receiver:0})}
            : await fetch(url,options);
        if (!response.ok) {--active;return response;}
        const json=response.json;
        return {...response,json:async () => {try {return await json();} finally {--active;}}};
    };
    const ui=await page(server);
    for (let i=0;i<paths.length;++i) {
        assert.deepEqual(pending.map(x=>x.url),paths.slice(0,i+1),'Settings loading must wait for the preceding response');
        if (i === 4) assert.equal(require('node:vm').runInContext('coreReady', ui.context), true);
        pending[i].gate.resolve(); await tick();
    }
    assert.equal(peak,1,'Startup settings must fit one HTTP slot alongside WebSockets');
    assert.equal(active,0);
    assert.equal(ui.element('btnSaveTuning').disabled,false,'Other settings remain usable after a failed request');
    console.log('PASS: bounded startup HTTP concurrency, response consumption and recovery from a failed settings endpoint');
})().catch(error=>{console.error(error);process.exitCode=1});
