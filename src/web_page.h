#pragma once

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Wasserraketen-Tracker</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{background:#0a0a1a;color:#e0e0e0;font-family:'Courier New',monospace;padding:12px}
h1{text-align:center;font-size:1.3em;color:#7af;margin-bottom:4px}
.record-bar{text-align:center;font-size:0.8em;color:#0ff;margin-bottom:6px}
.phase{text-align:center;padding:6px;border-radius:8px;font-size:1.2em;font-weight:bold;margin-bottom:10px;color:#000;transition:background 0.3s}
.phase-BEREIT{background:#666}.phase-SCHARF{background:#ff0}.phase-SCHUB{background:#f44}
.phase-STEIG{background:#fa0}.phase-FALLSCH{background:#f0f}.phase-LANDET{background:#0f0}
.stats{display:grid;grid-template-columns:repeat(3,1fr);gap:6px;margin-bottom:10px}
.stat{background:#12122a;border:1px solid #333;border-radius:6px;padding:8px;text-align:center}
.stat .label{font-size:0.7em;color:#888}
.stat .value{font-size:1.4em;font-weight:bold}
.stat .unit{font-size:0.7em;color:#888}
.green{color:#0f0}.cyan{color:#0ff}.yellow{color:#ff0}.orange{color:#fa0}.magenta{color:#f0f}.red{color:#f44}
.chart-wrap{background:#12122a;border:1px solid #333;border-radius:8px;padding:8px;margin-bottom:10px}
canvas{width:100%;height:300px;display:block}
.legend{display:flex;gap:12px;justify-content:center;font-size:0.75em;margin-top:4px}
.legend span::before{content:'';display:inline-block;width:12px;height:3px;margin-right:4px;vertical-align:middle}
.l-stable::before{background:#0f0}.l-wobble::before{background:#ff0}.l-tumble::before{background:#f44}

.rating-box{text-align:center;padding:12px;border-radius:10px;margin-bottom:10px;display:none}
.rating-text{font-size:2em;font-weight:bold;animation:pulse 0.6s ease-in-out infinite alternate}
.new-record{color:#ff0;font-size:1.1em;animation:glow 1s ease-in-out infinite alternate}
@keyframes pulse{from{transform:scale(1)}to{transform:scale(1.08)}}
@keyframes glow{from{text-shadow:0 0 5px #ff0}to{text-shadow:0 0 20px #ff0,0 0 40px #fa0}}

.summary{background:#0a1a0a;border:1px solid #0f0;border-radius:8px;padding:10px;text-align:center;font-size:0.9em;color:#0f0;display:none;margin-bottom:10px}

.lb{margin-bottom:10px}
.lb h2{font-size:1em;color:#7af;margin-bottom:6px;text-align:center}
.lb table{width:100%;border-collapse:collapse;font-size:0.75em}
.lb th{color:#888;text-align:center;padding:3px 4px;border-bottom:1px solid #333}
.lb td{text-align:center;padding:3px 4px;border-bottom:1px solid #1a1a2a}
.lb tr:first-child td{color:#ff0}
.lb tr:nth-child(2) td{color:#ccc}
.lb tr:nth-child(3) td{color:#a64}
</style>
</head>
<body>
<h1>&#127776; Wasserraketen-Tracker</h1>
<div class="record-bar" id="recordBar"></div>
<div class="phase phase-BEREIT" id="phase">BEREIT</div>

<div class="stats">
  <div class="stat"><div class="label">MAX H&Ouml;HE</div><div class="value cyan" id="maxAlt">0.0</div><div class="unit">m</div></div>
  <div class="stat"><div class="label">MAX TEMPO</div><div class="value cyan" id="maxSpd">0</div><div class="unit">km/h</div></div>
  <div class="stat"><div class="label">TEMPO 10m</div><div class="value magenta" id="spd10">0</div><div class="unit">km/h</div></div>
  <div class="stat"><div class="label">MAX G</div><div class="value orange" id="maxG">0.0</div><div class="unit">G</div></div>
  <div class="stat"><div class="label">FLUGZEIT</div><div class="value" id="time">0.0</div><div class="unit">s</div></div>
  <div class="stat"><div class="label">BIS SPITZE</div><div class="value yellow" id="tApogee">0.0</div><div class="unit">s</div></div>
  <div class="stat"><div class="label">SINKRATE</div><div class="value magenta" id="avgDesc">0.0</div><div class="unit">m/s</div></div>
  <div class="stat"><div class="label">DREHUNGEN</div><div class="value cyan" id="rolls">0</div><div class="unit">x</div></div>
  <div class="stat"><div class="label">TAUMELN</div><div class="value red" id="tumbles">0</div><div class="unit">x</div></div>
  <div class="stat"><div class="label">STABILIT&Auml;T</div><div class="value green" id="stab">100</div><div class="unit">%</div></div>
  <div class="stat"><div class="label">MAX SPIN</div><div class="value cyan" id="peakSpin">0</div><div class="unit">&deg;/s</div></div>
  <div class="stat"><div class="label">SCHIRM</div><div class="value" id="chute">-</div><div class="unit"></div></div>
  <div class="stat"><div class="label">FLUG #</div><div class="value" id="flightNum">0</div><div class="unit"></div></div>
</div>

<div class="rating-box" id="ratingBox">
  <div class="rating-text" id="ratingText"></div>
  <div class="new-record" id="newRecord" style="display:none">&#11088; NEUER REKORD! &#11088;</div>
</div>

<div class="chart-wrap">
  <canvas id="chart" height="300"></canvas>
  <div class="legend">
    <span class="l-stable">Stabil (&gt;70%)</span>
    <span class="l-wobble">Wackelig (40-70%)</span>
    <span class="l-tumble">Taumeln (&lt;40%)</span>
  </div>
</div>

<div class="summary" id="summary"></div>

<div class="lb" id="lbSection">
  <h2>&#127942; Bestenliste - Top Fl&uuml;ge</h2>
  <table>
    <thead><tr><th>#</th><th>Flug</th><th>H&ouml;he</th><th>Tempo</th><th>G</th><th>Stab%</th><th>Zeit</th></tr></thead>
    <tbody id="lbBody"></tbody>
  </table>
</div>

<script>
const $ = id => document.getElementById(id);
const phases = ['BEREIT','SCHARF','SCHUB','STEIG','FALLSCH','LANDET'];
let samples = [];
let canvas = $('chart');
let ctx = canvas.getContext('2d');

function resizeCanvas() {
  canvas.width = canvas.clientWidth * (window.devicePixelRatio || 1);
  canvas.height = canvas.clientHeight * (window.devicePixelRatio || 1);
  ctx.setTransform(window.devicePixelRatio || 1, 0, 0, window.devicePixelRatio || 1, 0, 0);
  drawChart();
}
window.addEventListener('resize', resizeCanvas);
resizeCanvas();

function stabColor(s) {
  if (s > 70) { let f=(100-s)/30; return `rgb(${Math.round(f*255)},255,0)`; }
  if (s > 40) { let f=(70-s)/30; return `rgb(255,${Math.round((1-f)*255)},0)`; }
  return '#f44';
}

function drawChart() {
  const W = canvas.clientWidth, H = canvas.clientHeight;
  ctx.clearRect(0, 0, W, H);

  if (samples.length < 2) {
    ctx.fillStyle = '#666';
    ctx.font = '14px Courier New';
    ctx.textAlign = 'center';
    ctx.fillText('Warte auf Flugdaten...', W/2, H/2);
    return;
  }

  const margin = {t:20, r:15, b:30, l:45};
  const gw = W - margin.l - margin.r;
  const gh = H - margin.t - margin.b;

  let maxAlt = 1, maxTime = 1;
  for (const s of samples) {
    if (s.alt > maxAlt) maxAlt = s.alt;
    if (s.t > maxTime) maxTime = s.t;
  }
  maxAlt *= 1.15;
  maxTime *= 1.05;

  const toX = t => margin.l + (t / maxTime) * gw;
  const toY = a => margin.t + gh - (a / maxAlt) * gh;

  // Gitter
  ctx.strokeStyle = '#1a1a3a';
  ctx.lineWidth = 1;
  for (let i = 0; i <= 5; i++) {
    let y = margin.t + (i * gh / 5);
    ctx.beginPath(); ctx.moveTo(margin.l, y); ctx.lineTo(margin.l + gw, y); ctx.stroke();
    ctx.fillStyle = '#666'; ctx.font = '10px Courier New'; ctx.textAlign = 'right';
    ctx.fillText((maxAlt * (5-i) / 5).toFixed(1), margin.l - 4, y + 3);
  }
  for (let i = 0; i <= 5; i++) {
    let x = margin.l + (i * gw / 5);
    ctx.beginPath(); ctx.moveTo(x, margin.t); ctx.lineTo(x, margin.t + gh); ctx.stroke();
    ctx.fillStyle = '#666'; ctx.font = '10px Courier New'; ctx.textAlign = 'center';
    ctx.fillText((maxTime * i / 5).toFixed(1) + 's', x, H - 8);
  }

  ctx.strokeStyle = '#444';
  ctx.beginPath(); ctx.moveTo(margin.l, margin.t); ctx.lineTo(margin.l, margin.t+gh); ctx.lineTo(margin.l+gw, margin.t+gh); ctx.stroke();

  ctx.lineWidth = 3;
  for (let i = 1; i < samples.length; i++) {
    const prev = samples[i-1], cur = samples[i];
    ctx.strokeStyle = stabColor(cur.stab);
    ctx.beginPath();
    ctx.moveTo(toX(prev.t), toY(prev.alt));
    ctx.lineTo(toX(cur.t), toY(cur.alt));
    ctx.stroke();
  }

  ctx.globalAlpha = 0.08;
  ctx.fillStyle = '#0ff';
  ctx.beginPath();
  ctx.moveTo(toX(samples[0].t), toY(0));
  for (const s of samples) ctx.lineTo(toX(s.t), toY(s.alt));
  ctx.lineTo(toX(samples[samples.length-1].t), toY(0));
  ctx.closePath();
  ctx.fill();
  ctx.globalAlpha = 1;

  // Spitze markieren
  let peak = samples.reduce((a, b) => b.alt > a.alt ? b : a);
  let px = toX(peak.t), py = toY(peak.alt);
  ctx.fillStyle = '#f44';
  ctx.beginPath(); ctx.arc(px, py, 5, 0, Math.PI*2); ctx.fill();
  ctx.fillStyle = '#f44'; ctx.font = 'bold 12px Courier New'; ctx.textAlign = 'left';
  ctx.fillText(peak.alt.toFixed(1) + 'm', px + 8, py - 2);

  // Fallschirm-Markierung
  for (let i = 1; i < samples.length; i++) {
    if (samples[i].alt < samples[i-1].alt - 0.1) {
      let cx = toX(samples[i].t), cy = toY(samples[i].alt);
      ctx.strokeStyle = '#f0f'; ctx.lineWidth = 1.5;
      ctx.beginPath(); ctx.arc(cx, cy-14, 8, Math.PI, 2*Math.PI); ctx.stroke();
      ctx.beginPath(); ctx.moveTo(cx-8, cy-14); ctx.lineTo(cx, cy); ctx.moveTo(cx+8, cy-14); ctx.lineTo(cx, cy); ctx.stroke();
      ctx.fillStyle = '#f0f'; ctx.font = '10px Courier New'; ctx.textAlign = 'center';
      ctx.fillText('SCHIRM', cx, cy-24);
      break;
    }
  }
}

function loadLeaderboard() {
  fetch('/leaderboard').then(r=>r.json()).then(data => {
    const tbody = $('lbBody');
    tbody.innerHTML = '';
    if (data.length === 0) {
      tbody.innerHTML = '<tr><td colspan="7" style="color:#666">Noch keine Flüge - los geht\'s!</td></tr>';
      return;
    }
    data.forEach((f, i) => {
      const tr = document.createElement('tr');
      tr.innerHTML = `<td>${i+1}</td><td>#${f.n}</td><td>${f.alt.toFixed(1)}m</td><td>${f.spd.toFixed(1)}</td><td>${f.g.toFixed(1)}</td><td>${f.stab.toFixed(0)}</td><td>${f.t.toFixed(1)}s</td>`;
      tbody.appendChild(tr);
    });
  }).catch(()=>{});
}
loadLeaderboard();

let prevPhase = 0;
let sseOk = false;
let pollTimer = null;

let lastArm = null;
function applyData(d) {
  // Scharfstellen erkannt (Arm-Zähler geändert) → frische Seite laden
  if (d.as !== undefined) {
    if (lastArm === null) lastArm = d.as;
    else if (d.as !== lastArm) { location.reload(); return; }
  }

  const phaseName = phases[d.p] || 'BEREIT';
  const el = $('phase');
  el.textContent = phaseName;
  el.className = 'phase phase-' + phaseName;

  $('maxAlt').textContent = d.ma.toFixed(1);
  $('maxSpd').textContent = (d.ms * 3.6).toFixed(0);
  $('spd10').textContent = ((d.s10 || 0) * 3.6).toFixed(0);
  $('maxG').textContent = (d.mg / 9.81).toFixed(2);
  $('time').textContent = d.t.toFixed(1);
  $('tApogee').textContent = (d.ta || 0).toFixed(1);
  $('avgDesc').textContent = (d.ad || 0).toFixed(1);
  $('rolls').textContent = d.rc || 0;
  $('tumbles').textContent = d.tc || 0;
  $('peakSpin').textContent = (d.mrr || 0).toFixed(0);
  let stab = d.stab || 100;
  $('stab').textContent = stab.toFixed(0);
  $('stab').style.color = stab > 70 ? '#0f0' : stab > 40 ? '#ff0' : '#f44';
  $('chute').textContent = d.ch ? 'OK' : '-';
  $('chute').style.color = d.ch ? '#f0f' : '#666';
  $('flightNum').textContent = d.fn || 0;

  if (d.ba > 0)
    $('recordBar').textContent = 'Rekord: ' + d.ba.toFixed(1) + 'm (Flug #' + d.bn + ') | Gesamt: ' + d.fn + ' Flüge';

  if (d.p >= 2 && d.p <= 4 && d.t > 0) {
    if (samples.length === 0 || d.t > samples[samples.length-1].t + 0.05)
      samples.push({t: d.t, alt: d.a, stab: d.is || 100});
  }
  if (d.p === 0) {
    samples = [];
    $('ratingBox').style.display = 'none';
    $('summary').style.display = 'none';
  }
  drawChart();

  if (d.p === 5) {
    let rb = $('ratingBox');
    rb.style.display = 'block';
    rb.style.background = stab > 45 ? '#0a2a0a' : '#2a1a0a';
    rb.style.border = '2px solid ' + (stab > 45 ? '#0f0' : '#fa0');
    $('ratingText').textContent = d.rat || 'GUT!';
    $('ratingText').style.color = stab > 45 ? '#0f0' : stab > 25 ? '#ff0' : '#fa0';
    $('newRecord').style.display = d.nr ? 'block' : 'none';
    $('summary').style.display = 'block';
    $('summary').innerHTML =
      'MAX HÖHE: ' + d.ma.toFixed(1) + 'm (' + (d.ma*3.28084).toFixed(0) + 'ft) | ' +
      'MAX TEMPO: ' + (d.ms*3.6).toFixed(0) + 'km/h<br>' +
      'MAX G: ' + (d.mg/9.81).toFixed(2) + 'G | ' +
      'FLUGZEIT: ' + d.t.toFixed(1) + 's | BIS SPITZE: ' + (d.ta||0).toFixed(1) + 's<br>' +
      'DREHUNGEN: ' + (d.rc||0) + ' | TAUMELN: ' + (d.tc||0) +
      ' | STABILITÄT: ' + stab.toFixed(0) + '%<br>' +
      'SINKRATE: ' + (d.ad||0).toFixed(1) + 'm/s | SCHIRM: ' + (d.ch?'OK':'NEIN');
    if (prevPhase !== 5) loadLeaderboard();
  }
  prevPhase = d.p;
}

// Polling fallback (used when SSE not available)
function startPolling() {
  if (pollTimer) return;
  pollTimer = setInterval(() => {
    fetch('/data').then(r => r.json()).then(applyData).catch(() => {});
  }, 1000);
}

// SSE — preferred; fall back to polling after 3 s of silence
let sseTimeout;
function connect() {
  if (typeof EventSource === 'undefined') { startPolling(); return; }
  const es = new EventSource('/events');
  es.onopen = () => { sseOk = true; };
  es.onmessage = e => {
    clearTimeout(sseTimeout);
    sseTimeout = setTimeout(() => { es.close(); connect(); }, 5000);
    try { applyData(JSON.parse(e.data)); } catch(ex) {}
  };
  es.onerror = () => {
    es.close();
    if (!sseOk) { startPolling(); return; }   // SSE never worked → poll
    sseOk = false;
    setTimeout(connect, 2000);
  };
}
connect();
// Always start polling as safety net — SSE will dominate when working
setTimeout(() => { if (!sseOk) startPolling(); }, 3000);
</script>
</body>
</html>
)rawliteral";
