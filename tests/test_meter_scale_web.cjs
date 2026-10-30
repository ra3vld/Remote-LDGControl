// Exercise the actual page script with two browsers and a board-state transport.
// Run: node tests/test_meter_scale_web.cjs
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const html = fs.readFileSync(path.join(__dirname, '../data/index.html'), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const tick = () => new Promise(resolve => setImmediate(resolve));
const deferred = () => {
    let resolve;
    const promise = new Promise(done => { resolve = done; });
    return { promise, resolve };
};

function board() {
    return {
        watts: 1000, revision: 0, sockets: [], posts: 0,
        snapshot() { return { meter_scale: this.watts, meter_scale_revision: this.revision }; },
        broadcast(data = this.snapshot()) {
            for (const socket of this.sockets) if (socket.opened) socket.message(data);
        },
        tap() {
            this.watts = this.watts === 1000 ? 100 : 1000;
            this.revision = (this.revision + 1) >>> 0;
            this.broadcast();
        },
        async fetch(url, options = {}) {
            if (url === '/api/ws-token') return { ok: true, json: async () => ({ token: 'test' }) };
            if (url === '/api/config') return { ok: true, json: async () => ({}) };
            if (url === '/api/status') {
                const data = { ...this.snapshot(), mqtt: { state: 'disabled', enabled: false,
                    message: 'Disabled', attempts: 0 } };
                if (this.holdStatus) await this.holdStatus.promise;
                return { ok: true, json: async () => data };
            }
            assert.equal(url, '/api/meter-scale');
            assert.equal(options.method, 'POST');
            assert.equal(options.headers['Content-Type'], 'application/json');
            assert.deepEqual(JSON.parse(options.body), { toggle: true });
            this.posts++;
            if (this.failPost) return { ok: false, status: 503 };
            this.tap();
            const data = this.snapshot();
            if (this.holdPost) await this.holdPost.promise;
            return { ok: true, json: async () => data };
        }
    };
}

async function page(server, initialSnapshot = true) {
    const elements = new Map();
    const context2d = new Proxy({}, { get: (target, key) => target[key] || (() => {}) });
    function element(id) {
        if (!elements.has(id)) elements.set(id, {
            textContent: '', style: {}, dataset: {}, width: 480, height: 320, events: {},
            classList: { add() {}, remove() {} },
            addEventListener(event, fn) { this.events[event] = fn; },
            getContext() { return context2d; }, scrollIntoView() {}
        });
        return elements.get(id);
    }
    class Socket {
        constructor() { server.sockets.push(this); }
        open() { this.opened = true; this.onopen(); }
        message(data) { this.onmessage({ data: JSON.stringify(data) }); }
        close() { this.opened = false; this.onclose(); }
    }
    let timerId = 0;
    let now = 0;
    const timers = new Map();
    const context = vm.createContext({
        document: { hidden: false, createElement: () => element(Symbol()), getElementById: element, querySelectorAll: () => [], querySelector: element },
        location: { protocol: 'http:', host: 'board.test' }, WebSocket: Socket, AbortController,
        fetch: server.fetch.bind(server),
        setTimeout(fn, delay) { const id = ++timerId; timers.set(id, { fn, at: now + delay }); return id; },
        clearTimeout(id) { timers.delete(id); }, console
    });
    vm.runInContext(script, context, { filename: 'index.html' });
    await tick();
    const socket = server.sockets.at(-1);
    socket.open();
    if (initialSnapshot) socket.message(server.snapshot());
    return {
        context, socket, element,
        async advance(ms) {
            now += ms;
            for (const [id, timer] of [...timers].sort((a, b) => a[1].at - b[1].at)) {
                if (timer.at <= now && timers.delete(id)) timer.fn();
            }
            await tick();
        },
        watts: () => vm.runInContext('meterState.maxPower', context),
        click: (id = 'meterCanvas') => element(id).events.click()
    };
}

async function run() {
    // Browser -> board -> second browser, then local touch -> both browsers.
    const shared = board();
    const a = await page(shared);
    const b = await page(shared);
    await a.click();
    assert.equal(shared.watts, 100);
    assert.equal(a.watts(), 100);
    assert.equal(b.watts(), 100);
    shared.tap();
    assert.equal(a.watts(), 1000);
    assert.equal(b.watts(), 1000);
    await b.click('scaleValue');
    assert.equal(a.watts(), 100);
    assert.equal(b.element('scaleValue').textContent, '100W');
    const reopened = await page(shared);
    assert.equal(reopened.watts(), 100);

    // A click before the first snapshot toggles the actual board value.
    const staleBoard = board();
    staleBoard.tap();
    staleBoard.holdStatus = deferred();
    const stale = await page(staleBoard, false);
    assert.equal(stale.watts(), 1000); // Initial HTML default, not authoritative.
    await stale.click();
    assert.equal(staleBoard.watts, 1000);
    assert.equal(stale.watts(), 1000);
    staleBoard.holdStatus.resolve();

    // Late HTTP acknowledgement cannot undo a newer display touch.
    shared.holdPost = deferred();
    const pending = a.click();
    await tick();
    const posts = shared.posts;
    await a.click(); // One request in flight; do not enqueue repeated taps.
    assert.equal(shared.posts, posts);
    shared.tap();
    shared.holdPost.resolve();
    await pending;
    assert.equal(a.watts(), shared.watts);
    assert.equal(b.watts(), shared.watts);
    delete shared.holdPost;

    // The periodic status fallback also respects the WebSocket revision.
    shared.holdStatus = deferred();
    const status = vm.runInContext('refreshMqttStatus()', a.context);
    await tick();
    shared.tap();
    shared.holdStatus.resolve();
    await status;
    assert.equal(a.watts(), shared.watts);
    delete shared.holdStatus;

    // Reconnect after a reboot accepts revision zero, ignoring old HTTP work.
    shared.holdPost = deferred();
    const beforeReboot = a.click();
    await tick();
    a.socket.close();
    shared.watts = 1000;
    shared.revision = 0;
    a.socket.open();
    shared.broadcast();
    shared.holdPost.resolve();
    await beforeReboot;
    assert.equal(a.watts(), 1000);
    delete shared.holdPost;

    // Sequence comparison survives uint32 rollover and rejects stale frames.
    a.socket.close();
    a.socket.open();
    a.socket.message({ meter_scale: 100, meter_scale_revision: 0xffffffff });
    a.socket.message({ meter_scale: 1000, meter_scale_revision: 0 });
    a.socket.message({ meter_scale: 100, meter_scale_revision: 0xfffffffe });
    assert.equal(a.watts(), 1000);
    a.socket.message({ meter_scale: 600, meter_scale_revision: 1 });
    a.socket.message({ meter_scale: 100, meter_scale_revision: '1' });
    assert.equal(a.watts(), 1000);

    // A rejected/offline request never fakes a successful local toggle.
    shared.failPost = true;
    const unchanged = a.watts();
    await a.click();
    assert.equal(a.watts(), unchanged);
    assert.match(a.element('toast').textContent, /Cannot change meter scale/);
    console.log('PASS: shared scale, two pages, display touch, reload, late HTTP/status, reconnect, rollover and failed requests');
}

module.exports = { board, page, tick, deferred };
if (require.main === module) run().catch(error => { console.error(error); process.exitCode = 1; });
