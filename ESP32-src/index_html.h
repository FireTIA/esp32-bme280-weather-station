// index_html.h

/*
 * ESP32 + BME280 — Климатическая станция с веб-дашбордом
 *   By: Qwen3.8-Max-Preview 
 *   От: 01.08.2026
 *   https://github.com/FireTIA/esp32-bme280-weather-station
 *   MIT License
 */

#pragma once

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>ESP32 Метеостанция</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4"></script>
<style>
:root{--bg:#0f1115;--card:#181c24;--accent:#FF8C00;--text:#e8ecf0;--muted:#8a94a0;--border:#262c38;--green:#34d399;--red:#f87171}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--text);font-family:system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;padding:16px;min-height:100vh}
h1{text-align:center;color:var(--accent);font-size:1.5rem;margin-bottom:20px;font-weight:700}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:14px;margin-bottom:20px}
.card{background:var(--card);border:1px solid var(--border);border-radius:14px;padding:18px;position:relative;overflow:hidden}
.card::before{content:'';position:absolute;top:0;left:0;right:0;height:3px;background:var(--accent)}
.card h3{font-size:.8rem;text-transform:uppercase;letter-spacing:.8px;color:var(--muted);margin-bottom:8px}
.card .val{font-size:2rem;font-weight:700;color:#fff}
.card .unit{font-size:.9rem;color:var(--accent);margin-left:4px}
.wifi-card{grid-column:1/-1}
.wifi-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:10px}
.wifi-item{background:#12151b;border-radius:10px;padding:10px 12px;border-left:3px solid var(--accent)}
.wifi-item .label{font-size:.7rem;color:var(--muted);text-transform:uppercase;letter-spacing:.5px}
.wifi-item .data{font-size:.95rem;margin-top:3px;font-weight:500}
.charts{display:grid;gap:16px;margin-top:20px}
.chart-box{background:var(--card);border:1px solid var(--border);border-radius:14px;padding:16px;position:relative}
.chart-box h4{font-size:.8rem;color:var(--muted);margin-bottom:10px;text-transform:uppercase;letter-spacing:.5px}
.chart-box canvas{width:100%!important;height:220px!important}
.offline-banner{display:none;background:#2a1a0a;border:1px solid var(--accent);border-radius:12px;padding:14px;text-align:center;color:var(--accent);margin-top:16px;font-size:.9rem}
.status-dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px;background:var(--red)}
.status-dot.ok{background:var(--green)}
@media(max-width:600px){.grid{grid-template-columns:1fr}.card .val{font-size:1.6rem}}
.head-link {color: inherit;text-decoration: none;transition: opacity 0.2s ease, text-decoration 0.2s ease;}
.head-link:hover {opacity: 0.8;text-decoration: underline;}
</style>
</head>
<body>
<h1>&#x1F321; <a href="/api/history" target="_blank" class="head-link">ESP32</a> <a href="https://github.com/FireTIA/esp32-bme280-weather-station" target="_blank" class="head-link">Метеостанция</a></h1>

<div class="grid">
  <div class="card">
    <h3>Температура</h3>
    <div><span class="val" id="v_t">--</span><span class="unit">&deg;C</span></div>
  </div>
  <div class="card">
    <h3>Влажность</h3>
    <div><span class="val" id="v_h">--</span><span class="unit">%</span></div>
  </div>
  <div class="card">
    <h3>Давление</h3>
    <div><span class="val" id="v_p">--</span><span class="unit">hPa</span></div>
  </div>
  <div class="card wifi-card">
    <h3><span class="status-dot" id="dot"></span>Wi-Fi &amp; Система</h3>
    <div class="wifi-grid">
      <div class="wifi-item"><div class="label">SSID</div><div class="data" id="w_ssid">--</div></div>
      <div class="wifi-item"><div class="label">Сигнал</div><div class="data" id="w_rssi">--</div></div>
      <div class="wifi-item"><div class="label">Канал</div><div class="data" id="w_ch">--</div></div>
      <div class="wifi-item"><div class="label">Uptime</div><div class="data" id="w_up">--</div></div>
    </div>
  </div>
</div>

<div class="charts" id="charts-wrap">
  <div class="chart-box"><h4>Температура (&deg;C) — 48ч</h4><canvas id="ch_t"></canvas></div>
  <div class="chart-box"><h4>Влажность (%) — 48ч</h4><canvas id="ch_h"></canvas></div>
  <div class="chart-box"><h4>Давление (hPa) — 48ч</h4><canvas id="ch_p"></canvas></div>
</div>
<div class="offline-banner" id="offline-msg">&#9888; Chart.js не загружен (офлайн). Графики недоступны, плитки работают.</div>

<script>
'use strict';
const $=id=>document.getElementById(id);

function fmtUp(s){const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60),sec=s%60;
return(d>0?d+'д ':'')+h+'ч '+m+'м '+sec+'с';}

function updateUI(d){
  if(d.temp!=null)$('v_t').textContent=d.temp.toFixed(1);
  if(d.hum!=null)$('v_h').textContent=d.hum.toFixed(1);
  if(d.press!=null)$('v_p').textContent=d.press.toFixed(1);
  $('w_ssid').textContent=d.wifi_ssid||'--';
  $('w_rssi').textContent=d.wifi_rssi_dbm+' dBm ('+d.wifi_signal_pct+'%)';
  $('w_ch').textContent=d.wifi_channel;
  $('w_up').textContent=fmtUp(d.uptime_sec);
  $('dot').className='status-dot'+(d.wifi_rssi_dbm!==0?' ok':'');
}

// --- WebSocket ---
let wsConn=null;
function connectWS(){
  wsConn=new WebSocket('ws://'+location.host+'/ws');
  wsConn.onmessage=e=>{try{updateUI(JSON.parse(e.data));}catch(x){}};
  wsConn.onclose=()=>setTimeout(connectWS,3000);
  wsConn.onerror=()=>wsConn.close();
}
connectWS();

// --- History Charts ---
function initCharts(){
  if(typeof Chart==='undefined'){
    $('charts-wrap').style.display='none';
    $('offline-msg').style.display='block';
    return;
  }
  Chart.defaults.color='#8a94a0';
  Chart.defaults.borderColor='#262c38';
  fetch('/api/history').then(r=>r.json()).then(j=>{
    const pts=j.points||[];
    if(!pts.length)return;
    const base=j.base_timestamp,intv=j.interval_sec;
    const labels=pts.map((_,i)=>{
      const d=new Date((base+i*intv)*1000);
      return d.toLocaleTimeString('ru-RU',{hour:'2-digit',minute:'2-digit',timeZone:'UTC'});
    });
    const mk=(id,label,color,data)=>new Chart($(id),{
      type:'line',
      data:{labels,datasets:[{label,data,borderColor:color,backgroundColor:color+'18',
        borderWidth:2,fill:true,pointRadius:0,tension:.3}]},
      options:{responsive:true,maintainAspectRatio:false,
        interaction:{mode:'index',intersect:false},
        plugins:{legend:{display:false}},
        scales:{x:{ticks:{maxTicksLimit:10,maxRotation:0}},y:{}}}
    });
    mk('ch_t','T','#FF8C00',pts.map(p=>p.t));
    mk('ch_h','H','#38bdf8',pts.map(p=>p.h));
    mk('ch_p','P','#34d399',pts.map(p=>p.p));
  }).catch(e=>console.warn('History load failed:',e));
}
initCharts();

// --- Initial status fetch ---
fetch('/api/status').then(r=>r.json()).then(updateUI).catch(()=>{});
</script>
</body>
</html>
)rawliteral";