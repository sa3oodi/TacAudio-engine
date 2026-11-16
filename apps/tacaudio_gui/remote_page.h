#pragma once
// The phone remote's web page (served by remote.cpp). Split into pieces: MSVC limits a single
// string literal to about 16 KB.

#include <string>

namespace tac {

inline const std::string& remotePageHtml() {
    static const std::string page = std::string(R"TAC(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover, user-scalable=no">
<meta name="theme-color" content="#0e1116">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="apple-mobile-web-app-title" content="TacAudio">
<link rel="manifest" href="/manifest.json">
<title>TacAudio Remote</title>
<style>
:root { --bg:#0e1116; --card:#191d25; --hi:#232935; --line:#2a303b; --text:#e9ecf1; --muted:#9aa3b2;
        --dim:#6b7383; --orange:#ff7a1a; --teal:#2fe3c4; --red:#ff5d5d; }
* { box-sizing:border-box; -webkit-tap-highlight-color:transparent; }
html, body { margin:0; background:var(--bg); color:var(--text);
  font:15px/1.4 -apple-system, "Segoe UI", Roboto, Inter, sans-serif; -webkit-user-select:none; user-select:none; }
main { max-width:520px; margin:0 auto; padding:calc(14px + env(safe-area-inset-top)) 16px calc(28px + env(safe-area-inset-bottom)); }
header { display:flex; align-items:center; gap:10px; margin-bottom:14px; }
.logo { width:30px; height:30px; border-radius:8px; background:#12151b; display:grid; place-items:center; }
.logo i { width:14px; height:14px; border-radius:50%; border:3px solid var(--orange); box-shadow:0 0 0 3px #12151b, 0 0 0 5px rgba(255,122,26,.6); }
h1 { font-size:18px; margin:0; font-weight:700; flex:1; }
.pill { font-size:12px; font-weight:600; color:var(--muted); background:var(--card); border:1px solid var(--line);
        padding:5px 10px; border-radius:999px; display:flex; align-items:center; gap:6px; white-space:nowrap; }
.pill b { width:8px; height:8px; border-radius:50%; background:var(--dim); }
.pill.ok b { background:#3ddc84; } .pill.wait b { background:#e8b931; } .pill.err b { background:var(--red); }
.card { background:var(--card); border:1px solid var(--line); border-radius:16px; padding:16px; margin-bottom:12px; }
.title { font-size:11px; font-weight:700; letter-spacing:.14em; text-transform:uppercase; color:var(--muted);
         display:flex; justify-content:space-between; align-items:center; margin-bottom:12px; }
.title span { letter-spacing:0; text-transform:none; font-size:13px; color:var(--text); font-weight:600; }
.big { display:grid; grid-template-columns:1fr 1fr; gap:10px; }
.bigbtn { border:1px solid var(--line); background:var(--hi); color:var(--text); border-radius:14px; padding:16px 12px;
          font:inherit; font-weight:700; font-size:16px; display:flex; flex-direction:column; align-items:center; gap:4px; }
.bigbtn small { font-weight:500; font-size:12px; color:var(--muted); }
.bigbtn.on { background:var(--orange); border-color:var(--orange); color:#1a0d02; }
.bigbtn.on small { color:#3a1c05; }
select { width:100%; background:var(--hi); color:var(--text); border:1px solid var(--line); border-radius:12px;
         padding:12px; font:inherit; font-weight:600; -webkit-appearance:none; appearance:none; }
.row { display:flex; align-items:center; justify-content:space-between; gap:12px; margin-top:10px; }
.row label { color:var(--muted); font-size:14px; }
.sw { width:52px; height:30px; border-radius:999px; background:#323845; position:relative; border:0; flex:none; }
.sw::after { content:""; position:absolute; top:3px; left:3px; width:24px; height:24px; border-radius:50%; background:#fff; transition:left .15s; }
.sw.on { background:var(--orange); } .sw.on::after { left:25px; }
.seg { display:grid; grid-auto-flow:column; grid-auto-columns:1fr; background:var(--hi); border-radius:12px; padding:3px; gap:3px; }
.seg button { border:0; background:transparent; color:var(--muted); font:inherit; font-weight:600; font-size:14px; padding:10px 4px; border-radius:9px; }
.seg button.on { background:var(--orange); color:#1a0d02; }
input[type=range] { -webkit-appearance:none; appearance:none; width:100%; height:36px; background:transparent; margin:0; }
input[type=range]::-webkit-slider-runnable-track { height:6px; border-radius:3px; background:linear-gradient(90deg,var(--orange) var(--p,50%),#323845 var(--p,50%)); }
input[type=range]::-webkit-slider-thumb { -webkit-appearance:none; width:26px; height:26px; border-radius:50%; background:#fff; margin-top:-10px; box-shadow:0 2px 6px rgba(0,0,0,.5); }
input[type=range]::-moz-range-track { height:6px; border-radius:3px; background:#323845; }
input[type=range]::-moz-range-progress { height:6px; border-radius:3px; background:var(--orange); }
input[type=range]::-moz-range-thumb { width:26px; height:26px; border-radius:50%; background:#fff; border:0; }
.val { font-variant-numeric:tabular-nums; font-weight:700; font-size:14px; min-width:64px; text-align:right; }
.meters { display:grid; gap:6px; }
.meter { display:flex; align-items:center; gap:8px; font-size:12px; color:var(--dim); }
.meter div { flex:1; height:8px; border-radius:4px; background:#232935; overflow:hidden; }
.meter div i { display:block; height:100%; width:0; background:linear-gradient(90deg,var(--teal),#e8d531 80%,var(--red)); transition:width .2s linear; }
.dis { opacity:.45; pointer-events:none; }
.msg { text-align:center; color:var(--muted); padding:40px 16px; display:none; }
.msg b { color:var(--text); display:block; font-size:18px; margin-bottom:8px; }
.foot { text-align:center; font-size:12px; color:var(--dim); margin-top:16px; }
</style>
</head>
)TAC") + R"TAC(<body>
<main>
  <header><div class="logo"><i></i></div><h1>TacAudio</h1><div class="pill" id="pill"><b></b><span id="status">Connecting...</span></div></header>

  <div class="msg" id="msg"><b id="msgTitle"></b><span id="msgText"></span></div>

  <div id="ui">
    <div class="big card" style="padding:12px">
      <button class="bigbtn" id="power"><span>Power</span><small id="powerSub">-</small></button>
      <button class="bigbtn" id="d3"><span>3D sound</span><small id="d3Sub">-</small></button>
    </div>

    <div class="card">
      <div class="title">Profile</div>
      <select id="profile"></select>
    </div>

    <div class="card">
      <div class="title">Footstep clarity <button class="sw" id="stepsOn" aria-label="Footstep clarity"></button></div>
      <div class="row" style="margin-top:0"><input type="range" id="steps" min="0" max="12" step="0.5"><span class="val" id="stepsVal"></span></div>
      <div class="row"><label>Smart (impacts only)</label><button class="sw" id="smart" aria-label="Smart"></button></div>
    </div>

    <div class="card">
      <div class="title">Combat dynamics</div>
      <div class="seg" id="dyn"><button data-v="off">Off</button><button data-v="light">Light</button><button data-v="medium">Medium</button><button data-v="strong">Strong</button></div>
    </div>

    <div class="card">
      <div class="title">Room</div>
      <div class="seg" id="room"><button data-v="neutral">Neutral</button><button data-v="balanced">Balanced</button><button data-v="spacious">Spacious</button></div>
    </div>

    <div class="card">
      <div class="title">Output</div>
      <div class="row" style="margin-top:0"><label>Volume</label><span class="val" id="volumeVal"></span></div>
      <input type="range" id="volume" min="-30" max="6" step="0.5">
      <div class="row" style="margin-top:4px"><label>Bass</label><span class="val" id="bassVal"></span></div>
      <input type="range" id="bass" min="-12" max="6" step="0.5">
      <div class="meters" style="margin-top:10px">
        <div class="meter">L<div><i id="mL"></i></div></div>
        <div class="meter">R<div><i id="mR"></i></div></div>
      </div>
    </div>
    <div class="foot" id="foot">TacAudio phone remote</div>
  </div>
</main>
)TAC" R"TAC(<script>
(function () {
  var key = '';
  var m = /[#&]k=([0-9a-f]{32})/.exec(location.hash);
  try {
    if (m) { key = m[1]; localStorage.setItem('tacKey', key); }
    else key = localStorage.getItem('tacKey') || '';
  } catch (e) { if (m) key = m[1]; }
  var $ = function (id) { return document.getElementById(id); };
  var state = null, busyUntil = {}, failures = 0;

  function show(title, text) {
    $('msg').style.display = title ? 'block' : 'none';
    $('ui').style.display = title ? 'none' : 'block';
    $('msgTitle').textContent = title || '';
    $('msgText').textContent = text || '';
  }
  function pill(cls, text) { $('pill').className = 'pill ' + cls; $('status').textContent = text; }
  if (!key) { show('Scan the QR code', 'Open TacAudio on your PC, go to Settings > Phone remote and scan the QR code with this phone.'); pill('err', 'Not paired'); return; }

  function send(name, value) {
    busyUntil[name] = Date.now() + 1200;
    fetch('/api/set?name=' + encodeURIComponent(name) + '&value=' + encodeURIComponent(value),
          { method: 'POST', headers: { 'X-Key': key } }).catch(function () {});
  }
  function busy(name) { return (busyUntil[name] || 0) > Date.now(); }
  function fmt(v, signed) { return (signed && v > 0 ? '+' : '') + Number(v).toFixed(1) + ' dB'; }
  function fill(el) { var p = (el.value - el.min) / (el.max - el.min) * 100; el.style.setProperty('--p', p + '%'); }
  function setSw(el, on) { el.classList.toggle('on', !!on); }
  function setSeg(el, v) { [].forEach.call(el.children, function (b) { b.classList.toggle('on', b.dataset.v === v); }); }
  function meter(el, db) { var w = Math.max(0, Math.min(1, (db + 60) / 60)) * 100; el.style.width = w + '%'; }

  $('power').onclick = function () { if (state) { state.on = !state.on; send('power', state.on ? 1 : 0); render(); } };
  $('d3').onclick = function () { if (state) { state.d3 = !state.d3; send('3d', state.d3 ? 1 : 0); render(); } };
  $('profile').onchange = function () { send('profile', this.value); };
  $('stepsOn').onclick = function () { if (state) { state.stepsOn = !state.stepsOn; send('stepsOn', state.stepsOn ? 1 : 0); render(); } };
  $('smart').onclick = function () { if (state) { state.smart = !state.smart; send('smart', state.smart ? 1 : 0); render(); } };
  [['steps', false], ['volume', false], ['bass', true]].forEach(function (s) {
    var el = $(s[0]), name = s[0], signed = s[1];
    el.oninput = function () {
      fill(el); $(name + 'Val').textContent = name === 'steps' ? '+' + Number(el.value).toFixed(1) + ' dB' : fmt(el.value, signed);
      busyUntil[name] = Date.now() + 1200;
      clearTimeout(el._t); el._t = setTimeout(function () { send(name, el.value); }, 60);
    };
  });
  [['dyn', 'dyn'], ['room', 'room']].forEach(function (s) {
    $(s[0]).onclick = function (e) {
      var v = e.target.dataset && e.target.dataset.v;
      if (!v || !state) return;
      state[s[1]] = v; send(s[1], v); render();
    };
  });

  function render() {
    var s = state;
    $('power').classList.toggle('on', s.on); $('powerSub').textContent = s.on ? 'On' : 'Off';
    $('d3').classList.toggle('on', s.d3); $('d3Sub').textContent = s.d3 ? 'On' : 'Off (stereo)';
    var sel = $('profile');
    if (sel.options.length !== s.profiles.length || [].some.call(sel.options, function (o, i) { return o.text !== s.profiles[i]; })) {
      sel.innerHTML = '';
      s.profiles.forEach(function (n, i) { var o = document.createElement('option'); o.value = i; o.text = n; sel.appendChild(o); });
    }
    if (!busy('profile') && document.activeElement !== sel) sel.value = s.profile;
    setSw($('stepsOn'), s.stepsOn); setSw($('smart'), s.smart);
    if (!busy('steps')) { $('steps').value = s.steps; fill($('steps')); $('stepsVal').textContent = '+' + Number(s.steps).toFixed(1) + ' dB'; }
    $('steps').parentNode.classList.toggle('dis', !s.stepsOn); $('smart').parentNode.classList.toggle('dis', !s.stepsOn);
    setSeg($('dyn'), s.dyn); setSeg($('room'), s.room);
    if (!busy('volume')) { $('volume').value = s.volume; fill($('volume')); $('volumeVal').textContent = fmt(s.volume, true); }
    if (!busy('bass')) { $('bass').value = s.bass; fill($('bass')); $('bassVal').textContent = fmt(s.bass, true); }
    meter($('mL'), s.l); meter($('mR'), s.r);
    $('foot').textContent = 'TacAudio ' + s.version + (s.game ? ' - ' + s.game : '');
    if (!s.on) pill('', 'Off');
    else if (s.playing) pill('ok', s.surround ? 'Playing 7.1' : 'Playing');
    else pill('wait', 'Waiting for game');
  }

  function poll() {
    fetch('/api/state', { headers: { 'X-Key': key }, cache: 'no-store' }).then(function (r) {
      if (r.status === 403) { show('Scan the new QR code', 'This phone\'s code is no longer valid (a new code was made in TacAudio). Scan the QR code in Settings > Phone remote again.'); pill('err', 'Not paired'); throw 0; }
      return r.json();
    }).then(function (s) {
      failures = 0; show(''); state = s; render();
    }).catch(function (e) {
      if (e === 0) return;
      if (++failures > 3) { show('Can\'t reach your PC', 'Make sure TacAudio is running, Phone remote is on, and this phone is on the same Wi-Fi.'); pill('err', 'Offline'); }
    }).then(function () { setTimeout(poll, document.hidden ? 2000 : 250); });
  }
  poll();
})();
</script>
</body>
</html>
)TAC";
    return page;
}

}  // namespace tac
