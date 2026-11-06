// Build tools run on the host. The ESP32 only streams the resulting files.
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const zlib = require('node:zlib');
const acorn = require('acorn');
const postcss = require('postcss');
const terser = require('terser');

const root = path.resolve(__dirname, '..');
const minifyOptions = { ecma: 2020, compress: { passes: 2 }, mangle: { toplevel: false },
    toplevel: false, format: { comments: false, ascii_only: false, inline_script: true } };
const read = name => fs.readFileSync(path.join(root, name), 'utf8');
const gzip = data => zlib.gzipSync(data, { level: 9, mtime: 0 });

function minifyCss(source) {
    const tree = postcss.parse(source);
    tree.walkComments(node => node.remove());
    tree.walk(node => {
        node.raws.before = ''; node.raws.after = '';
        if (node.type === 'decl') node.raws.between = ':';
        if (node.type === 'rule' || node.type === 'atrule') node.raws.between = '';
        if (node.nodes) node.raws.semicolon = false;
    });
    tree.raws.after = ''; tree.raws.semicolon = false;
    return tree.toString();
}

function minifyHtml(source) {
    return source.replace(/^[ \t]+/gm, '').replace(/<!--[^]*?-->/g, '').replace(/>\s+</g, '><').trim();
}

function criticalTranslations(code, html, messages) {
    const strings = new Set();
    function visit(node) {
        if (!node || typeof node !== 'object') return;
        if (node.type === 'Literal' && typeof node.value === 'string') strings.add(node.value);
        for (const [key, value] of Object.entries(node)) {
            if (key === 'start' || key === 'end') continue;
            if (Array.isArray(value)) value.forEach(visit);
            else if (value && typeof value === 'object') visit(value);
        }
    }
    visit(acorn.parse(code, { ecmaVersion: 'latest' }));
    for (const match of html.matchAll(/data-i18n(?:>|\s[^>]*>)([^<]*)</g)) strings.add(match[1]);
    for (const match of html.matchAll(/(?:title|placeholder|aria-label)="([^"]*)"/g)) strings.add(match[1]);
    return Object.fromEntries(Object.entries(messages).filter(([key]) => strings.has(key)));
}

async function build(outDirectory) {
    const destination = path.resolve(outDirectory);
    const files = new Map(), manifest = {};
    const add = (name, text) => {
        const data = Buffer.from(text);
        files.set(name, data); files.set(name + '.gz', gzip(data));
        return data;
    };
    const script = async (name, source) => {
        const result = await terser.minify(source, minifyOptions);
        const data = Buffer.from(result.code);
        const digest = crypto.createHash('sha256').update(data).digest();
        const filename = name + '.' + digest.toString('hex').slice(0, 12) + '.js';
        add(filename, data);
        manifest[name] = { file: filename, integrity: 'sha256-' + digest.toString('base64') };
    };
    let html = read('data/index.html');
    const core = ['web/staging.js', 'web/core.js', 'web/boot.js'].map(read).join('\n');
    const messages = JSON.parse(read('web/messages.ru.json'));
    const seed = criticalTranslations(core, html, messages);
    const rest = Object.fromEntries(Object.entries(messages).filter(([key]) => !Object.hasOwn(seed, key)));
    await script('ru', 'if(assetIsCurrent("ru")){Object.assign(languageMessages,' + JSON.stringify(rest) + ');localeReady=true;refreshLanguage();}');
    await script('widgets', `(() => { if (!assetIsCurrent('widgets')) return;
        const observers = [];
        let stopWidgets = () => observers.forEach(observer => observer.disconnect());
        try {
        document.getElementById('historyBody').innerHTML = ${JSON.stringify(minifyHtml(read('web/widgets.html')))};
        ${read('web/widgets.js')}
        stopWidgets = () => {
            clearTimeout(historyTimer);
            canvas.removeEventListener('click', toggleMeterScale);
            document.removeEventListener?.('visibilitychange', scheduleHistoryDraw);
            observers.forEach(observer => observer.disconnect());
        };
        bindLanguageText(document.getElementById('historyBody'));
        installVisuals({drawMeter, drawHistory, scheduleHistoryDraw,
            stop: stopWidgets});
    } catch (_) { try { stopWidgets(); } catch (_) {} failVisuals(); } })();`);
    await script('settings', `(() => { if (!assetIsCurrent('settings')) return;
        document.getElementById('advancedSettingsBody').innerHTML = ${JSON.stringify(minifyHtml(read('web/settings.html')))};
        ${read('web/settings.js')}
        installSettings({toggleStaticIP, updateBrightnessControls, calibrateBrightness,
            loadBrightnessSettings, saveBrightnessSettings, loadRadioSettings, saveRadioSettings,
            readDisplayOrientation, saveDisplayOrientation, saveSettings, resetSettings,
            saveAntennaSettings, applySettingsForm, applyAntennaTable});
    })();`);
    const boot = await terser.minify('const assetManifest=' + JSON.stringify(manifest) +
        ';const languageMessages=' + JSON.stringify(seed) + ';\n' + core, minifyOptions);
    // Minify markup before injecting scripts/styles: never rewrite JS strings as HTML.
    html = minifyHtml(html.replace('<!-- LDG_STYLE -->', '__LDG_STYLE__')
        .replace('<!-- LDG_CORE -->', '__LDG_CORE__'));
    html = html.replace(/\s*__LDG_STYLE__\s*/,  '<style>' + minifyCss(read('web/style.css')) + '</style>');
    html = html.replace(/\s*__LDG_CORE__\s*/,  '<script>' + boot.code + '</script>');
    add('index.html', html);
    fs.mkdirSync(destination, { recursive: true });
    // A build directory is private to its environment. Remove stale hashed assets.
    for (const name of fs.readdirSync(destination))
        if (/^(?:index\.html|(?:ru|widgets|settings)\.[a-f0-9]{12}\.js)(?:\.gz)?$/.test(name) && !files.has(name))
            fs.unlinkSync(path.join(destination, name));
    for (const [name, data] of files) fs.writeFileSync(path.join(destination, name), data);
    for (const entry of fs.readdirSync(path.join(root, 'data'), { withFileTypes: true })) {
        if (entry.name === 'index.html') continue;
        fs.cpSync(path.join(root, 'data', entry.name), path.join(destination, entry.name), { recursive: true });
    }
    const sizes = Object.fromEntries([...files].map(([name, data]) => [name, data.length]));
    const report = { assets: manifest, sizes, criticalTranslationCount: Object.keys(seed).length,
        totalTranslationCount: Object.keys(messages).length };
    fs.writeFileSync(destination + '.json', JSON.stringify(report, null, 2) + '\n');
    return report;
}

module.exports = { build };
if (require.main === module) {
    const index = process.argv.indexOf('--out');
    const destination = index >= 0 && process.argv[index + 1];
    if (!destination) { console.error('Usage: node scripts/build_web.cjs --out <directory>'); process.exitCode = 1; }
    else build(destination).then(report => console.log(JSON.stringify(report, null, 2)))
        .catch(error => { console.error(error); process.exitCode = 1; });
}
