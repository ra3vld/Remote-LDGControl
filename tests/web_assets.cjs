// Every test exercises generated production assets, including split scripts.
const fs = require('node:fs');
const path = require('node:path');
const directory = path.resolve(__dirname, '../.pio/web-test-assets');
// Generate first: node scripts/build_web.cjs --out .pio/web-test-assets
if (!fs.existsSync(directory + '.json')) throw new Error('Build web test assets first');
const report = JSON.parse(fs.readFileSync(directory + '.json', 'utf8'));
const html = fs.readFileSync(path.join(directory, 'index.html'), 'utf8');
function asset(url) {
    const name = new URL(url, 'http://ldg.test').searchParams.get('asset');
    if (!name) return null;
    if (!Object.values(report.assets).some(spec => spec.file === name)) throw new Error('Unknown asset: ' + name);
    return fs.readFileSync(path.join(directory, name), 'utf8');
}
async function serveAsset(route) {
    const body = asset(route.request().url());
    if (body === null) return false;
    await route.fulfill({ contentType: 'text/javascript', body });
    return true;
}
module.exports = { html, asset, report, directory, serveAsset };
