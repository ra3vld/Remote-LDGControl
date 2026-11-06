const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const crypto = require('node:crypto');
const zlib = require('node:zlib');
const vm = require('node:vm');
const { build } = require('../scripts/build_web.cjs');
const { html, report, directory } = require('./web_assets.cjs');
(async () => {
    const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'ldg-web-assets-'));
    try {
        const second = await build(path.join(temporary, 'assets'));
        assert.deepEqual(second, report, 'Production output must be reproducible');
        for (const name of Object.keys(report.sizes)) {
            const data = fs.readFileSync(path.join(directory, name));
            assert.deepEqual(data, fs.readFileSync(path.join(temporary, 'assets', name)));
            if (name.endsWith('.gz')) assert.deepEqual(zlib.gunzipSync(data), fs.readFileSync(path.join(directory, name.slice(0, -3))));
        }
        for (const spec of Object.values(report.assets)) {
            const data = fs.readFileSync(path.join(directory, spec.file));
            const hash = crypto.createHash('sha256').update(data).digest();
            assert.ok(spec.file.includes(hash.toString('hex').slice(0, 12)));
            assert.equal(spec.integrity, 'sha256-' + hash.toString('base64'));
            new vm.Script(data.toString());
        }
        assert.ok(!/<script[^>]+src=|<link[^>]+href=/i.test(html), 'Core must not wait for dependencies');
        assert.match(html, /id="controlActions" disabled/, 'Truncated core must leave actions disabled');
        new vm.Script(html.match(/<script>([^]*?)<\/script>/)[1]);
        console.log('PASS: reproducible minification, gzip round trips, hashed assets/SRI, standalone core and disabled incomplete shell');
    } finally { fs.rmSync(temporary, { recursive: true, force: true }); }
})().catch(error => { console.error(error); process.exitCode = 1; });
