const canvas = document.getElementById('meterCanvas');

let ctx = canvas.getContext('2d');
if (!ctx) throw new Error('Canvas unavailable');

let meterFace = null;

let meterFaceKey = '';

let lastMeterDrawKey = '';

function drawMeterFace() {
            const w = canvas.width;
            const h = canvas.height;
            const pad = 40;

            // Cross-needle geometry mirrors src/display_ui.cpp: each needle
            // pivots at a bottom corner and sweeps its own 90° arc with
            // radius = needle length. Because arm = 2·halfBase, each arc
            // passes through the other pivot at zero (needle resting
            // horizontal) and the two arcs cross near top-center at full.
            const fwdPx = w - pad;          // forward pivot, bottom-right
            const fwdPy = h - pad;
            const refPx = pad;              // reflected pivot, bottom-left
            const refPy = h - pad;
            const arm   = h - 2 * pad;      // needle length = arc radius
            const halfBase = (w - 2 * pad) / 2;

            ctx.clearRect(0, 0, w, h);
            ctx.fillStyle = '#0b0c10';
            ctx.fillRect(0, 0, w, h);

            // SWR locus curves under the needles
            drawSWRCurves(fwdPx, fwdPy, refPx, refPy, arm);

            // Per-pivot scale arcs. Canvas convention: 0=E, π/2=S, π=W, 3π/2=N.
            // Forward arc:  π (left/zero)  → 3π/2 (up/full)  in the upper-left
            //                                                quadrant of fwd pivot
            // Reflected arc: 3π/2 (up/full) → 2π (right/zero) in the upper-right
            //                                                quadrant of ref pivot
            ctx.lineWidth = 2;
            ctx.strokeStyle = 'rgba(34,197,94,0.55)';
            ctx.beginPath();
            ctx.arc(fwdPx, fwdPy, arm, Math.PI, 1.5 * Math.PI);
            ctx.stroke();
            ctx.strokeStyle = 'rgba(239,68,68,0.55)';
            ctx.beginPath();
            ctx.arc(refPx, refPy, arm, 1.5 * Math.PI, 2 * Math.PI);
            ctx.stroke();

            // 8 ticks per arc (every 12.5%); major every 25% with labels.
            // Ticks radiate from each pivot, labels sit just inside the arc.
            function drawScale(pivotX, pivotY, phiZero, phiFull, color) {
                const N = 8;
                for (let i = 0; i <= N; i++) {
                    const f = i / N;
                    const phi = phiZero + (phiFull - phiZero) * f;
                    const cos = Math.cos(phi);
                    const sin = Math.sin(phi);
                    const major = (i % 2 === 0);
                    const tickLen = major ? 14 : 7;
                    ctx.strokeStyle = major ? color : '#666';
                    ctx.lineWidth = major ? 2 : 1;
                    ctx.beginPath();
                    ctx.moveTo(pivotX + cos * (arm + 2),
                               pivotY + sin * (arm + 2));
                    ctx.lineTo(pivotX + cos * (arm + 2 + tickLen),
                               pivotY + sin * (arm + 2 + tickLen));
                    ctx.stroke();
                    // Skip the 100% per-arc label; shared "max" label below
                    // sits above the arc crossing.
                    if (major && i < N) {
                        const val = powerTick(powerAtFraction(f));
                        const lx = pivotX + cos * (arm - 22);
                        const ly = pivotY + sin * (arm - 22);
                        ctx.fillStyle = color;
                        ctx.font = 'bold 18px -apple-system, sans-serif';
                        ctx.textAlign = 'center';
                        ctx.textBaseline = 'middle';
                        ctx.fillText(val.toString(), lx, ly);
                    }
                }
            }
            drawScale(fwdPx, fwdPy, Math.PI,     1.5 * Math.PI, '#22c55e');
            drawScale(refPx, refPy, 2 * Math.PI, 1.5 * Math.PI, '#ef4444');

            // The two arcs meet at the midline, at y = pivotY − √(arm² − hb²).
            // Shared "max scale" label + "WATTS" caption float just above it.
            const cx = w / 2;
            const crossY = fwdPy - Math.sqrt(arm * arm - halfBase * halfBase);
            ctx.fillStyle = '#ccc';
            ctx.font = 'bold 18px -apple-system, sans-serif';
            ctx.textAlign = 'center';
            ctx.textBaseline = 'middle';
            ctx.fillText(meterState.maxPower.toString(), cx, crossY - 22);
            ctx.fillStyle = '#888';
            ctx.font = '12px -apple-system, sans-serif';
            ctx.fillText(t('WATTS'), cx, crossY - 40);

            // Dim pivot rings (drawn before needles so the bright dots stack on top)
            ctx.fillStyle = '#166534';
            ctx.beginPath();
            ctx.arc(fwdPx, fwdPy, 10, 0, 2 * Math.PI);
            ctx.fill();
            ctx.fillStyle = '#7f1d1d';
            ctx.beginPath();
            ctx.arc(refPx, refPy, 10, 0, 2 * Math.PI);
            ctx.fill();

        }

function drawMeter() {
            const w = canvas.width, h = canvas.height;
            const faceKey = [w, h, meterState.maxPower, meterState.logarithmic, language].join(':');
            const drawKey = [faceKey, meterState.fwdPower, meterState.refPower, meterState.swr].join(':');
            if (drawKey === lastMeterDrawKey) return;
            if (!meterFace || meterFaceKey !== faceKey) {
                meterFace = document.createElement('canvas');
                meterFace.width = w;
                meterFace.height = h;
                const destination = ctx;
                try {
                    ctx = meterFace.getContext('2d');
                    drawMeterFace();
                } finally { ctx = destination; }
                meterFaceKey = faceKey;
            }
            ctx.clearRect(0, 0, w, h);
            ctx.drawImage(meterFace, 0, 0);
            const pad = 40, arm = h - 2 * pad, cx = w / 2;
            const fwdPx = w - pad, fwdPy = h - pad, refPx = pad, refPy = h - pad;
            lastMeterDrawKey = drawKey;
            // Needles: fixed length = arm, tip walks along the per-pivot arc.
            //   Forward:  φ = π     + fwdFrac·(π/2)    (left → up)
            //   Reflected:φ = 2π − refFrac·(π/2)       (right → up)
            const fwdFrac = powerFraction(meterState.fwdPower);
            const refFrac = powerFraction(meterState.refPower);
            const fwdTipPhi = Math.PI + fwdFrac * (Math.PI / 2);
            const refTipPhi = 2 * Math.PI - refFrac * (Math.PI / 2);
            const fwdTipX = fwdPx + arm * Math.cos(fwdTipPhi);
            const fwdTipY = fwdPy + arm * Math.sin(fwdTipPhi);
            const refTipX = refPx + arm * Math.cos(refTipPhi);
            const refTipY = refPy + arm * Math.sin(refTipPhi);

            ctx.lineCap = 'round';
            ctx.strokeStyle = '#22c55e';
            ctx.lineWidth = 4;
            ctx.beginPath();
            ctx.moveTo(fwdPx, fwdPy);
            ctx.lineTo(fwdTipX, fwdTipY);
            ctx.stroke();

            ctx.strokeStyle = '#ef4444';
            ctx.beginPath();
            ctx.moveTo(refPx, refPy);
            ctx.lineTo(refTipX, refTipY);
            ctx.stroke();

            // Bright pivot dots over the needles
            ctx.fillStyle = '#22c55e';
            ctx.beginPath();
            ctx.arc(fwdPx, fwdPy, 5, 0, 2 * Math.PI);
            ctx.fill();
            ctx.fillStyle = '#ef4444';
            ctx.beginPath();
            ctx.arc(refPx, refPy, 5, 0, 2 * Math.PI);
            ctx.fill();

            // Power readouts under each pivot
            ctx.textBaseline = 'bottom';
            ctx.fillStyle = '#22c55e';
            ctx.font = 'bold 22px -apple-system, sans-serif';
            ctx.textAlign = 'right';
            ctx.fillText(meterState.fwdPower === null ? '--' : meterState.fwdPower.toFixed(1) + 'W', fwdPx - 10, fwdPy + 35);
            ctx.fillStyle = '#ef4444';
            ctx.textAlign = 'left';
            ctx.fillText(meterState.refPower === null ? '--' : meterState.refPower.toFixed(1) + 'W', refPx + 10, refPy + 35);

            // Numeric SWR readout between the power values.
            const swr = meterState.swr;
            ctx.fillStyle = swr === null ? '#888'
                : swr > 2.0 ? '#ef4444' : swr > 1.5 ? '#f59e0b' : '#22c55e';
            ctx.font = 'bold 24px -apple-system, sans-serif';
            ctx.textAlign = 'center';
            ctx.fillText(t('SWR ') + (swr === null ? '--' : swr.toFixed(1) + ':1'), cx, fwdPy + 35);
        }

function drawSWRCurves(fwdPx, fwdPy, refPx, refPy, arm) {
            const swrValues = [1.5, 2, 3, 5, 10];
            const colors = ['#84cc16', '#eab308', '#f97316', '#ef4444', '#a855f7'];

            // For each SWR ratio: refPower/fwdPower = γ². Sweep fwdFrac and
            // plot the locus of needle intersections. Each needle is a line
            // from its own pivot at length=arm along its sweep angle.
            for (let si = 0; si < swrValues.length; si++) {
                const swr = swrValues[si];
                const gamma = (swr - 1) / (swr + 1);
                const gammaSq = gamma * gamma;

                ctx.strokeStyle = colors[si];
                ctx.lineWidth = 1.5;
                ctx.globalAlpha = 0.5;
                ctx.beginPath();

                let first = true;
                let lastPt = null;
                for (let i = 1; i <= 100; i++) {
                    const power = (i / 100) * meterState.maxPower;
                    const fwdFrac = powerFraction(power);
                    const refFrac = powerFraction(power * gammaSq);

                    const fwdPhi = Math.PI + fwdFrac * (Math.PI / 2);
                    const refPhi = 2 * Math.PI - refFrac * (Math.PI / 2);
                    const fwdTx = fwdPx + arm * Math.cos(fwdPhi);
                    const fwdTy = fwdPy + arm * Math.sin(fwdPhi);
                    const refTx = refPx + arm * Math.cos(refPhi);
                    const refTy = refPy + arm * Math.sin(refPhi);

                    const ix = lineIntersect(fwdPx, fwdPy, fwdTx, fwdTy,
                                              refPx, refPy, refTx, refTy);
                    if (ix) {
                        if (first) { ctx.moveTo(ix.x, ix.y); first = false; }
                        else { ctx.lineTo(ix.x, ix.y); }
                        lastPt = ix;
                    }
                }
                ctx.stroke();
                ctx.globalAlpha = 1;

                // Label the curve at its low-power end (further from the
                // pivot apex, more readable).
                if (lastPt) {
                    ctx.fillStyle = colors[si];
                    ctx.font = '14px -apple-system, sans-serif';
                    ctx.textAlign = 'center';
                    ctx.textBaseline = 'middle';
                    ctx.globalAlpha = 0.85;
                    ctx.fillText(swr + ':1', lastPt.x, lastPt.y - 10);
                    ctx.globalAlpha = 1;
                }
            }
        }

function lineIntersect(x1, y1, x2, y2, x3, y3, x4, y4) {
            const denom = (x1 - x2) * (y3 - y4) - (y1 - y2) * (x3 - x4);
            if (Math.abs(denom) < 0.001) return null;
            const t = ((x1 - x3) * (y3 - y4) - (y1 - y3) * (x3 - x4)) / denom;
            return {
                x: x1 + t * (x2 - x1),
                y: y1 + t * (y2 - y1)
            };
        }

canvas.addEventListener('click', toggleMeterScale);

const historyColors = ['#2563eb', '#15803d', '#b45309', '#7c3aed', '#be185d', '#0e7490'];

let historyTimer = null;

let historyDrawPending = false;

let historyView = null;

function scheduleHistoryDraw() {
            if (historyDrawPending && historyVisible && !document.hidden) return;
            clearTimeout(historyTimer);
            historyTimer = null;
            historyDrawPending = false;
            if (!historyVisible || document.hidden) return;
            // Coalesce live updates and pointer events; idle/disconnected charts
            // still advance so a stale value is never extended indefinitely.
            historyTimer = setTimeout(globalThis.drawHistory, 80);
            historyDrawPending = true;
        }

function historyClock(at) {
            return new Date(at).toLocaleTimeString(language === 'ru' ? 'ru-RU' : 'en-GB', { hour12: false });
        }

function drawHistory() {
            clearTimeout(historyTimer);
            historyTimer = null;
            historyDrawPending = false;
            if (!historyVisible || document.hidden) return;
            const end = Date.now(), start = end - historyWindow;
            pruneHistory(end);
            historyView = { start, end };
            const samples = historySamples.filter(sample => sample.at >= start - historyGap);
            const at = historyCursor === null ? end : Math.max(start, Math.min(end, historyCursor));
            const selected = historyAt(at);
            for (const [id, key, label] of [['historyFwd', 'fwd', 'FWD'], ['historyRef', 'ref', 'REF'],
                ['historySwr', 'swr', t('SWR')], ['historyMode', 'mode', t('Mode')],
                ['historyAntenna', 'antenna', t('Antenna')], ['historyBand', 'band', t('Band')]]) {
                document.getElementById(id).textContent = label + ': ' + historyLabel(selected, key);
            }
            document.getElementById('historyTime').textContent = historySamples.length
                ? historyClock(at) + (selected ? '' : ' · ' + t('No data at this time')) : t('Waiting for history data…');

            for (const id of ['historyPower', 'historySWR', 'historyStates']) {
                const element = document.getElementById(id);
                const width = element.clientWidth, height = element.clientHeight;
                if (!width || !height) continue;
                const ratio = Math.min(globalThis.devicePixelRatio || 1, 2);
                if (element.width !== Math.round(width * ratio) || element.height !== Math.round(height * ratio)) {
                    element.width = Math.round(width * ratio);
                    element.height = Math.round(height * ratio);
                }
                const pen = element.getContext('2d');
                pen.setTransform(ratio, 0, 0, ratio, 0, 0);
                pen.clearRect(0, 0, width, height);
                const left = 76, right = width - 8, top = 12, bottom = height - 25;
                const x = time => left + (time - start) / historyWindow * (right - left);
                pen.font = '11px sans-serif';
                pen.fillStyle = '#9ca3af';
                pen.strokeStyle = '#2a2d3e';
                for (let i = 0; i <= 2; i++) {
                    const px = left + (right - left) * i / 2;
                    pen.beginPath(); pen.moveTo(px, top); pen.lineTo(px, bottom); pen.stroke();
                    pen.textAlign = i === 0 ? 'left' : i === 2 ? 'right' : 'center';
                    pen.fillText(historyClock(start + historyWindow * i / 2), px, height - 5);
                }
                pen.save();
                pen.beginPath(); pen.rect(left, top, right - left, bottom - top); pen.clip();
                let axis = null;
                if (id === 'historyStates') {
                    const rows = ['mode', 'antenna', 'band'], rowHeight = (bottom - top) / 3;
                    rows.forEach((key, row) => {
                        const runs = [];
                        samples.forEach((sample, i) => {
                            if (sample[key] === null) return;
                            const until = Math.min(end, sample.at + historyGap, samples[i + 1]?.at ?? end);
                            if (until <= start) return;
                            const label = historyLabel(sample, key);
                            const previous = runs[runs.length - 1];
                            if (previous && previous.end === sample.at && previous.label === label) previous.end = until;
                            else runs.push({ start: sample.at, end: until, label, sample });
                        });
                        for (const run of runs) {
                            const x1 = Math.max(left, x(run.start)), x2 = x(run.end), y = top + row * rowHeight + 2;
                            const index = key === 'band' ? antennaBands.indexOf(run.sample.band) : run.sample[key];
                            pen.fillStyle = historyColors[index % historyColors.length];
                            if (key === 'band' && run.sample.held) pen.fillStyle = '#854d0e';
                            pen.fillRect(x1, y, Math.max(x2 - x1, 1), rowHeight - 4);
                            pen.fillStyle = '#fff'; pen.textAlign = 'left';
                            if (pen.measureText(run.label).width + 8 < x2 - x1) pen.fillText(run.label, x1 + 4, y + rowHeight / 2 + 2);
                        }
                    });
                } else {
                    const keys = id === 'historyPower' ? ['fwd', 'ref'] : ['swr'];
                    let maximum = id === 'historyPower' ? 1 : 2;
                    for (const sample of samples) for (const key of keys) {
                        if (sample.at >= start && sample[key] !== null) maximum = Math.max(maximum, sample[key]);
                    }
                    const step = 10 ** Math.floor(Math.log10(maximum));
                    maximum = Math.ceil(maximum / step) * step;
                    const minimum = id === 'historyPower' ? 0 : 1;
                    axis = { minimum, maximum };
                    const y = value => bottom - (value - minimum) / (maximum - minimum) * (bottom - top - 3);
                    for (let i = 0; i <= 2; i++) {
                        const py = bottom - (bottom - top - 3) * i / 2;
                        pen.strokeStyle = '#2a2d3e';
                        pen.beginPath(); pen.moveTo(left, py); pen.lineTo(right, py); pen.stroke();
                    }
                    keys.forEach(key => {
                        pen.strokeStyle = key === 'fwd' ? '#4ade80' : key === 'ref' ? '#fb923c' : '#60a5fa';
                        pen.fillStyle = pen.strokeStyle; pen.lineWidth = 1.7;
                        pen.beginPath();
                        let previous = null;
                        for (const sample of samples) {
                            if (sample[key] === null) { previous = null; continue; }
                            const px = x(sample.at), py = y(sample[key]);
                            if (!previous || sample.at - previous.at > historyGap) pen.moveTo(px, py);
                            else pen.lineTo(px, py);
                            // Isolated measurements remain visible as dots.
                            pen.fillRect(px - 1, py - 1, 2, 2);
                            previous = sample;
                        }
                        pen.stroke();
                    });
                }
                pen.restore();
                if (axis) {
                    pen.fillStyle = '#9ca3af'; pen.textAlign = 'right';
                    for (let i = 0; i <= 2; i++) {
                        const value = axis.minimum + (axis.maximum - axis.minimum) * i / 2;
                        pen.fillText(value < 10 ? value.toFixed(1) : String(Math.round(value)), left - 5,
                            bottom - (bottom - top - 3) * i / 2 + 3);
                    }
                }
                if (id === 'historyStates') {
                    pen.fillStyle = '#9ca3af'; pen.textAlign = 'right';
                    [t('Mode'), t('Antenna'), t('Band')].forEach((label, row) => {
                        pen.fillText(label, left - 8, top + (row + 0.5) * (bottom - top) / 3 + 4);
                    });
                }
                if (historyCursor !== null) {
                    pen.strokeStyle = '#e8e8f0'; pen.lineWidth = 1;
                    pen.beginPath(); pen.moveTo(x(at), top); pen.lineTo(x(at), bottom); pen.stroke();
                }
            }
            historyTimer = setTimeout(globalThis.drawHistory, 1000);
        }

for (const id of ['historyPower', 'historySWR', 'historyStates']) {
            const element = document.getElementById(id);
            const inspect = event => {
                if (!historyView) return;
                const bounds = element.getBoundingClientRect();
                const fraction = Math.max(0, Math.min(1, (event.clientX - bounds.left - 76) / (bounds.width - 84)));
                historyCursor = historyView.start + fraction * historyWindow;
                scheduleHistoryDraw();
            };
            element.addEventListener('pointermove', inspect);
            element.addEventListener('pointerdown', inspect);
            element.addEventListener('pointerleave', event => {
                if (event.pointerType === 'touch') return;
                historyCursor = null;
                scheduleHistoryDraw();
            });
            if (typeof ResizeObserver !== 'undefined') {
                const observer = new ResizeObserver(scheduleHistoryDraw);
                observer.observe(element); observers.push(observer);
            }
        }

document.addEventListener?.('visibilitychange', scheduleHistoryDraw);
