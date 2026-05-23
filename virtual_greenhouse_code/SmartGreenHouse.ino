
#include <WiFi.h>
#include <PubSubClient.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>

// ─── WiFi Credentials ────────────────────────────────────────
const char* WIFI_SSID = "e3f27";
const char* WIFI_PASS = "55667788";

// ─── MQTT Broker ─────────────────────────────────────────────
const char* MQTT_BROKER   = "broker.hivemq.com";
const int   MQTT_PORT     = 1883;
const char* MQTT_CLIENT   = "ESP32_Greenhouse";

// ─── MQTT Topics ─────────────────────────────────────────────
// Publish
const char* TOPIC_MOISTURE = "LMa_greenhouse/sensors/moisture";
const char* TOPIC_LIGHT    = "LMa_greenhouse/sensors/light";
const char* TOPIC_PEST     = "LMa_greenhouse/sensors/pest";
// Subscribe
const char* TOPIC_IRRIGATION = "LMa_greenhouse/actuators/irrigation";
const char* TOPIC_SPRAYER    = "LMa_greenhouse/actuators/sprayer";
const char* TOPIC_GROWLIGHT  = "LMa_greenhouse/actuators/growlight";
const char* TOPIC_MODE       = "LMa_greenhouse/mode";

// ─── Objects ─────────────────────────────────────────────────
WiFiClient     wifiClient;
PubSubClient   mqtt(wifiClient);
AsyncWebServer server(80);

// ─── Sensor State ────────────────────────────────────────────
float moisture      = 65.0f;   // %
int   lightLux      = 400;     // lux
bool  pestDetected  = false;

// ─── Actuator State (software-only) ──────────────────────────
bool irrigationOn   = false;
bool sprayerOn      = false;
bool growlightOn    = false;

// ─── Timed actuator auto-off ─────────────────────────────────
unsigned long irrigationEndMs = 0;   // 0 = not running
unsigned long sprayerEndMs    = 0;

// ─── Mode ────────────────────────────────────────────────────
bool autoMode = true;   // true = AUTO, false = MANUAL

// ─── Moisture history ring buffer (for chart) ────────────────
#define HISTORY_LEN 30
float moistureHistory[HISTORY_LEN];
int   historyIndex = 0;
bool  historyFull  = false;

// ─── Pest detection timing ───────────────────────────────────
const unsigned long PEST_COOLDOWN_MS = 30000;  // 30 s between events
unsigned long lastPestMs = 0;
float pestChancePerSec   = 0.03f;             // 3 % chance each second

// ─── Publish interval ────────────────────────────────────────
const unsigned long PUBLISH_INTERVAL_MS = 2000;
unsigned long lastPublishMs = 0;

// ─── Simulated clock (seconds since boot, wraps daily) ───────
// We simulate a 24-hour day compressed into real time for demo
// 1 real minute = 1 simulated hour  → full day in 24 real minutes
const float SIMULATED_DAY_REAL_SECONDS = 24.0f * 60.0f;  // 24 min

// ─────────────────────────────────────────────────────────────
//  HTML Dashboard
// ─────────────────────────────────────────────────────────────
// Stored in PROGMEM to save SRAM
const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1.0"/>
<title>🌿 Smart Greenhouse</title>
<link rel="preconnect" href="https://fonts.googleapis.com"/>
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin/>
<link href="https://fonts.googleapis.com/css2?family=DM+Serif+Display:ital@0;1&family=DM+Mono:wght@400;500&family=Outfit:wght@300;400;500;600&display=swap" rel="stylesheet"/>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js"></script>
<style>
/* ── Reset & base ─────────────────────────── */
*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
:root{
  --bg:#0d1117;
  --surface:#161b22;
  --surface2:#1c2330;
  --border:#2d3748;
  --green:#4ade80;
  --green-dim:#166534;
  --amber:#fbbf24;
  --red:#f87171;
  --blue:#60a5fa;
  --text:#e2e8f0;
  --text-dim:#718096;
  --card-shadow:0 4px 24px rgba(0,0,0,.45);
  --radius:16px;
}
html{font-size:16px}
body{
  background:var(--bg);
  color:var(--text);
  font-family:'Outfit',sans-serif;
  min-height:100vh;
  overflow-x:hidden;
}

/* ── Decorative grid bg ───────────────────── */
body::before{
  content:'';
  position:fixed;inset:0;
  background-image:
    linear-gradient(rgba(74,222,128,.04) 1px,transparent 1px),
    linear-gradient(90deg,rgba(74,222,128,.04) 1px,transparent 1px);
  background-size:48px 48px;
  pointer-events:none;
  z-index:0;
}

/* ── Header ───────────────────────────────── */
header{
  position:relative;z-index:1;
  padding:2rem 2.5rem 1.5rem;
  display:flex;align-items:center;justify-content:space-between;
  border-bottom:1px solid var(--border);
  background:linear-gradient(180deg,rgba(74,222,128,.06) 0%,transparent 100%);
}
.logo{display:flex;align-items:center;gap:.75rem}
.logo-icon{
  width:44px;height:44px;
  background:linear-gradient(135deg,#16a34a,#4ade80);
  border-radius:12px;
  display:flex;align-items:center;justify-content:center;
  font-size:22px;
  box-shadow:0 0 24px rgba(74,222,128,.3);
}
h1{
  font-family:'DM Serif Display',serif;
  font-size:1.6rem;
  font-weight:400;
  letter-spacing:-.02em;
  color:#fff;
}
h1 span{color:var(--green)}
.header-right{display:flex;align-items:center;gap:1.5rem}
#connection-badge{
  display:flex;align-items:center;gap:.4rem;
  font-family:'DM Mono',monospace;
  font-size:.72rem;
  color:var(--text-dim);
}
.dot{
  width:8px;height:8px;border-radius:50%;
  background:var(--red);
  transition:background .4s;
}
.dot.online{background:var(--green);box-shadow:0 0 8px var(--green)}

/* ── Mode toggle ──────────────────────────── */
.mode-toggle{
  display:flex;align-items:center;gap:.6rem;
  font-size:.8rem;font-weight:500;
}
.mode-label{color:var(--text-dim);font-size:.75rem;text-transform:uppercase;letter-spacing:.08em}
.toggle-track{
  position:relative;
  width:56px;height:28px;
  background:var(--surface2);
  border:1px solid var(--border);
  border-radius:99px;
  cursor:pointer;
  transition:background .3s;
}
.toggle-track.auto{background:var(--green-dim);border-color:var(--green)}
.toggle-thumb{
  position:absolute;top:3px;left:3px;
  width:20px;height:20px;
  background:#fff;border-radius:50%;
  transition:transform .3s cubic-bezier(.34,1.56,.64,1);
  box-shadow:0 1px 4px rgba(0,0,0,.4);
}
.toggle-track.auto .toggle-thumb{transform:translateX(28px);background:var(--green)}
.mode-text{
  font-family:'DM Mono',monospace;
  font-size:.75rem;
  color:var(--green);
  width:46px;
  transition:color .3s;
}
.mode-text.manual{color:var(--amber)}

/* ── Main grid ────────────────────────────── */
main{
  position:relative;z-index:1;
  padding:2rem 2.5rem;
  display:grid;
  grid-template-columns:repeat(auto-fill,minmax(320px,1fr));
  gap:1.25rem;
  max-width:1400px;
  margin:0 auto;
}

/* ── Cards ────────────────────────────────── */
.card{
  background:var(--surface);
  border:1px solid var(--border);
  border-radius:var(--radius);
  padding:1.5rem;
  box-shadow:var(--card-shadow);
  transition:border-color .3s,box-shadow .3s;
}
.card:hover{border-color:#3d4f66;box-shadow:0 8px 32px rgba(0,0,0,.5)}
.card-header{
  display:flex;align-items:center;justify-content:space-between;
  margin-bottom:1.25rem;
}
.card-title{
  font-size:.7rem;font-weight:500;
  text-transform:uppercase;letter-spacing:.1em;
  color:var(--text-dim);
}
.card-icon{font-size:1.1rem}

/* ── Gauge ────────────────────────────────── */
.gauge-wrap{display:flex;flex-direction:column;align-items:center;gap:.5rem}
.gauge{
  position:relative;
  width:160px;height:88px;
  overflow:hidden;
}
.gauge svg{width:100%;height:auto}
.gauge-bg{fill:none;stroke:var(--surface2);stroke-width:14;stroke-linecap:round}
.gauge-fill{
  fill:none;stroke-width:14;stroke-linecap:round;
  transform-origin:80px 80px;
  transition:stroke-dashoffset .8s cubic-bezier(.4,0,.2,1),stroke .5s;
}
/* circumference of r=66 semicircle = π*66 ≈ 207.3 */
.gauge-fill{stroke-dasharray:207.3;stroke-dashoffset:207.3}
.gauge-value{
  position:absolute;bottom:0;left:50%;transform:translateX(-50%);
  font-family:'DM Serif Display',serif;
  font-size:2rem;color:#fff;
  white-space:nowrap;
}
.gauge-unit{font-size:.85rem;color:var(--text-dim);margin-left:2px}

/* ── Light bar ────────────────────────────── */
.light-bar-wrap{margin-top:.5rem}
.light-label-row{display:flex;justify-content:space-between;font-size:.72rem;color:var(--text-dim);margin-bottom:.4rem}
.light-track{
  height:14px;
  background:var(--surface2);
  border-radius:99px;
  overflow:hidden;
  position:relative;
}
.light-fill{
  height:100%;
  background:linear-gradient(90deg,#1e3a8a,#60a5fa,#fde68a);
  border-radius:99px;
  transition:width .8s cubic-bezier(.4,0,.2,1);
  position:relative;
}
.light-fill::after{
  content:'';
  position:absolute;top:0;left:0;right:0;bottom:0;
  background:linear-gradient(90deg,transparent 60%,rgba(255,255,255,.2));
  border-radius:inherit;
}
.light-value{
  font-family:'DM Serif Display',serif;
  font-size:2.4rem;color:#fff;
  margin-top:.75rem;
}
.light-value span{font-family:'Outfit',sans-serif;font-size:.85rem;color:var(--text-dim)}

/* ── Pest card ────────────────────────────── */
.pest-status{
  display:flex;align-items:center;gap:1rem;
  padding:1rem 1.25rem;
  border-radius:12px;
  background:var(--surface2);
  border:1px solid var(--border);
  transition:all .4s;
}
.pest-status.detected{
  background:rgba(248,113,113,.08);
  border-color:rgba(248,113,113,.4);
}
.pest-icon{
  font-size:2rem;
  transition:filter .3s;
}
.pest-status.detected .pest-icon{
  filter:drop-shadow(0 0 8px var(--red));
  animation:pulse-pest 1s ease-in-out infinite;
}
@keyframes pulse-pest{
  0%,100%{transform:scale(1);opacity:1}
  50%{transform:scale(1.15);opacity:.8}
}
.pest-info{}
.pest-label{font-size:.72rem;color:var(--text-dim);text-transform:uppercase;letter-spacing:.08em}
.pest-state{
  font-size:1.1rem;font-weight:600;
  color:var(--green);
  transition:color .3s;
}
.pest-status.detected .pest-state{color:var(--red)}

/* ── Actuator grid ────────────────────────── */
.actuator-grid{
  grid-column:1/-1;
  display:grid;
  grid-template-columns:repeat(auto-fill,minmax(200px,1fr));
  gap:1rem;
}
.act-btn{
  display:flex;flex-direction:column;align-items:center;justify-content:center;
  gap:.6rem;
  padding:1.5rem 1rem;
  border-radius:var(--radius);
  border:2px solid var(--border);
  background:var(--surface);
  cursor:pointer;
  transition:all .25s;
  user-select:none;
  position:relative;
  overflow:hidden;
}
.act-btn::before{
  content:'';
  position:absolute;inset:0;
  background:radial-gradient(circle at 50% 50%,rgba(255,255,255,.04),transparent 70%);
  opacity:0;
  transition:opacity .3s;
}
.act-btn:hover:not(.disabled)::before{opacity:1}
.act-btn:hover:not(.disabled){transform:translateY(-2px);box-shadow:0 8px 24px rgba(0,0,0,.4)}
.act-btn:active:not(.disabled){transform:translateY(0)}
.act-btn.on{
  border-color:var(--green);
  box-shadow:0 0 20px rgba(74,222,128,.15);
}
.act-btn.on .act-icon{filter:drop-shadow(0 0 6px currentColor)}
.act-btn.disabled{opacity:.4;cursor:not-allowed}
.act-icon{font-size:2.2rem;transition:filter .3s}
.act-label{font-size:.8rem;font-weight:500;text-transform:uppercase;letter-spacing:.08em;color:var(--text-dim)}
.act-state{
  font-family:'DM Mono',monospace;
  font-size:.7rem;
  padding:.2rem .6rem;
  border-radius:99px;
  background:var(--surface2);
  color:var(--text-dim);
  transition:all .3s;
}
.act-btn.on .act-state{
  background:rgba(74,222,128,.15);
  color:var(--green);
}

/* ── Chart card ───────────────────────────── */
.chart-card{grid-column:1/-1}
.chart-wrap{height:160px;position:relative;margin-top:.5rem}

/* ── Toast ────────────────────────────────── */
#toast{
  position:fixed;bottom:1.5rem;right:1.5rem;
  background:var(--surface);
  border:1px solid var(--border);
  border-radius:10px;
  padding:.75rem 1.25rem;
  font-size:.8rem;
  color:var(--text);
  box-shadow:0 8px 32px rgba(0,0,0,.5);
  opacity:0;
  transform:translateY(12px);
  transition:opacity .3s,transform .3s;
  pointer-events:none;
  z-index:100;
}
#toast.show{opacity:1;transform:translateY(0)}

/* ── Responsive tweaks ────────────────────── */
@media(max-width:600px){
  header{flex-direction:column;gap:1rem;padding:1.25rem}
  main{padding:1rem;gap:1rem}
  .header-right{flex-wrap:wrap;justify-content:center}
}
</style>
</head>
<body>

<header>
  <div class="logo">
    <div class="logo-icon">🌿</div>
    <div>
      <h1>Smart<span>House</span></h1>
      <div style="font-size:.7rem;color:var(--text-dim);font-family:'DM Mono',monospace">Virtual Greenhouse v1.0</div>
    </div>
  </div>
  <div class="header-right">
    <div id="connection-badge">
      <div class="dot" id="conn-dot"></div>
      <span id="conn-label">Connecting…</span>
    </div>
    <div class="mode-toggle">
      <span class="mode-label">Mode</span>
      <div class="toggle-track" id="mode-track" onclick="toggleMode()">
        <div class="toggle-thumb"></div>
      </div>
      <span class="mode-text" id="mode-text">AUTO</span>
    </div>
  </div>
</header>

<main>

  <!-- Moisture Gauge -->
  <div class="card">
    <div class="card-header">
      <span class="card-title">Soil Moisture</span>
      <span class="card-icon">💧</span>
    </div>
    <div class="gauge-wrap">
      <div class="gauge">
        <svg viewBox="0 0 160 88" xmlns="http://www.w3.org/2000/svg">
          <path class="gauge-bg" d="M14 80 A66 66 0 0 1 146 80"/>
          <path class="gauge-fill" id="moisture-arc" stroke="#4ade80" d="M14 80 A66 66 0 0 1 146 80"/>
        </svg>
        <div class="gauge-value" id="moisture-val">--<span class="gauge-unit">%</span></div>
      </div>
    </div>
  </div>

  <!-- Light Sensor -->
  <div class="card">
    <div class="card-header">
      <span class="card-title">Light Level</span>
      <span class="card-icon">☀️</span>
    </div>
    <div class="light-value" id="light-big">-- <span>lux</span></div>
    <div class="light-bar-wrap">
      <div class="light-label-row"><span>Night</span><span>Bright</span></div>
      <div class="light-track">
        <div class="light-fill" id="light-fill" style="width:0%"></div>
      </div>
    </div>
  </div>

  <!-- Pest Status -->
  <div class="card">
    <div class="card-header">
      <span class="card-title">Pest Monitor</span>
      <span class="card-icon">🔬</span>
    </div>
    <div class="pest-status" id="pest-status">
      <div class="pest-icon" id="pest-icon">🛡️</div>
      <div class="pest-info">
        <div class="pest-label">Detection Status</div>
        <div class="pest-state" id="pest-state">Clear</div>
      </div>
    </div>
  </div>

  <!-- Actuators -->
  <div class="actuator-grid">
    <div class="act-btn" id="btn-irrigation" onclick="sendControl('irrigation')">
      <div class="act-icon" style="color:#60a5fa">💧</div>
      <div class="act-label">Irrigation</div>
      <div class="act-state" id="state-irrigation">OFF</div>
    </div>
    <div class="act-btn" id="btn-sprayer" onclick="sendControl('sprayer')">
      <div class="act-icon" style="color:#a78bfa">🌫️</div>
      <div class="act-label">Pest Sprayer</div>
      <div class="act-state" id="state-sprayer">OFF</div>
    </div>
    <div class="act-btn" id="btn-growlight" onclick="sendControl('growlight')">
      <div class="act-icon" style="color:#fbbf24">💡</div>
      <div class="act-label">Grow Light</div>
      <div class="act-state" id="state-growlight">OFF</div>
    </div>
  </div>

  <!-- Moisture History Chart -->
  <div class="card chart-card">
    <div class="card-header">
      <span class="card-title">Moisture History (last 30 readings)</span>
      <span class="card-icon">📈</span>
    </div>
    <div class="chart-wrap">
      <canvas id="moistChart"></canvas>
    </div>
  </div>

</main>

<div id="toast"></div>

<script>
// ── Chart setup ───────────────────────────────────────────
const ctx = document.getElementById('moistChart').getContext('2d');
const chart = new Chart(ctx,{
  type:'line',
  data:{
    labels:[],
    datasets:[{
      label:'Moisture %',
      data:[],
      borderColor:'#4ade80',
      borderWidth:2,
      backgroundColor:'rgba(74,222,128,.08)',
      fill:true,
      tension:.4,
      pointRadius:0,
    }]
  },
  options:{
    responsive:true,maintainAspectRatio:false,
    animation:{duration:400},
    plugins:{legend:{display:false}},
    scales:{
      x:{display:false},
      y:{
        min:0,max:100,
        grid:{color:'rgba(255,255,255,.04)'},
        ticks:{color:'#718096',font:{family:'DM Mono',size:10}}
      }
    }
  }
});

// ── State ─────────────────────────────────────────────────
let isAuto = true;
let reading = 0;

// ── Gauge math ────────────────────────────────────────────
// Semicircle arc length ≈ 207.3; offset = arc*(1-pct)
function setGauge(pct){
  const arc = 207.3;
  const offset = arc*(1-Math.min(1,Math.max(0,pct)));
  const el = document.getElementById('moisture-arc');
  el.style.strokeDashoffset = offset;
  // Color gradient: red→amber→green
  const h = Math.round(pct*120); // 0=red, 120=green
  el.style.stroke = `hsl(${h},80%,55%)`;
}

// ── Fetch & update ─────────────────────────────────────────
async function fetchStatus(){
  try{
    const r = await fetch('/api/status');
    if(!r.ok) throw new Error('bad status');
    const d = await r.json();

    // Connection badge
    document.getElementById('conn-dot').className = 'dot online';
    document.getElementById('conn-label').textContent = 'Connected';

    // Moisture
    const m = parseFloat(d.moisture);
    document.getElementById('moisture-val').innerHTML =
      Math.round(m)+'<span class="gauge-unit">%</span>';
    setGauge(m/100);

    // Light
    const l = parseInt(d.light);
    document.getElementById('light-big').innerHTML =
      l+'<span> lux</span>';
    document.getElementById('light-fill').style.width = (l/10)+'%';

    // Pest
    const pest = d.pest === '1' || d.pest === 1 || d.pest === true;
    const ps = document.getElementById('pest-status');
    document.getElementById('pest-icon').textContent = pest ? '🦟' : '🛡️';
    document.getElementById('pest-state').textContent = pest ? 'PEST DETECTED!' : 'Clear';
    ps.className = 'pest-status' + (pest ? ' detected' : '');

    // Actuators
    setActuator('irrigation', d.irrigation === 'ON' || d.irrigation === true);
    setActuator('sprayer',    d.sprayer    === 'ON' || d.sprayer    === true);
    setActuator('growlight',  d.growlight  === 'ON' || d.growlight  === true);

    // Mode
    isAuto = d.mode === 'auto';
    document.getElementById('mode-track').className = 'toggle-track' + (isAuto ? ' auto' : '');
    document.getElementById('mode-text').textContent = isAuto ? 'AUTO' : 'MANUAL';
    document.getElementById('mode-text').className   = 'mode-text' + (isAuto ? '' : ' manual');
    // Disable buttons in auto mode
    ['irrigation','sprayer','growlight'].forEach(k=>{
      document.getElementById('btn-'+k).classList.toggle('disabled', isAuto);
    });

    // Chart
    const hist = d.history;
    if(hist && Array.isArray(hist)){
      chart.data.labels = hist.map((_,i)=>i);
      chart.data.datasets[0].data = hist;
      chart.update('none');
    }

  } catch(e){
    document.getElementById('conn-dot').className = 'dot';
    document.getElementById('conn-label').textContent = 'Offline';
  }
}

function setActuator(name, on){
  const btn = document.getElementById('btn-'+name);
  const lbl = document.getElementById('state-'+name);
  btn.className = 'act-btn' + (on ? ' on' : '') + (isAuto ? ' disabled' : '');
  lbl.textContent = on ? 'ON' : 'OFF';
}

// ── Control ───────────────────────────────────────────────
async function sendControl(device){
  if(isAuto) return;
  const btn = document.getElementById('btn-'+device);
  const isOn = btn.classList.contains('on');
  const newState = isOn ? 'OFF' : 'ON';
  try{
    const r = await fetch('/api/control?device='+device+'&state='+newState);
    const t = await r.text();
    showToast(device.toUpperCase()+' → '+newState);
    fetchStatus();
  } catch(e){ showToast('Error sending command','error'); }
}

async function toggleMode(){
  const newMode = isAuto ? 'manual' : 'auto';
  try{
    await fetch('/api/control?device=mode&state='+newMode);
    showToast('Switched to '+(newMode.toUpperCase())+' mode');
    fetchStatus();
  } catch(e){}
}

// ── Toast ─────────────────────────────────────────────────
let toastTimer;
function showToast(msg){
  const t = document.getElementById('toast');
  t.textContent = msg;
  t.className = 'show';
  clearTimeout(toastTimer);
  toastTimer = setTimeout(()=>{ t.className=''; },2500);
}

// ── Poll ──────────────────────────────────────────────────
fetchStatus();
setInterval(fetchStatus, 1000);
</script>
</body>
</html>
)rawliteral";


// ─────────────────────────────────────────────────────────────
//  MQTT Callback
// ─────────────────────────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Build null-terminated string from payload
  char msg[16] = {0};
  unsigned int copyLen = min(length, (unsigned int)(sizeof(msg) - 1));
  memcpy(msg, payload, copyLen);
  msg[copyLen] = '\0';

  Serial.printf("[MQTT] Topic: %s  Payload: %s\n", topic, msg);

  if (strcmp(topic, TOPIC_IRRIGATION) == 0) {
    if (!autoMode) irrigationOn = (strcmp(msg, "ON") == 0);
  } else if (strcmp(topic, TOPIC_SPRAYER) == 0) {
    if (!autoMode) sprayerOn = (strcmp(msg, "ON") == 0);
  } else if (strcmp(topic, TOPIC_GROWLIGHT) == 0) {
    if (!autoMode) growlightOn = (strcmp(msg, "ON") == 0);
  } else if (strcmp(topic, TOPIC_MODE) == 0) {
    autoMode = (strcmp(msg, "auto") == 0);
  }
}

// ─────────────────────────────────────────────────────────────
//  WiFi Setup
// ─────────────────────────────────────────────────────────────
void setupWiFi() {
  Serial.printf("\n[WiFi] Connecting to %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.setAutoReconnect(true);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
    if (millis() - start > 15000) {
      Serial.println("\n[WiFi] Timeout — rebooting…");
      ESP.restart();
    }
  }
  Serial.printf("\n[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
}

// ─────────────────────────────────────────────────────────────
//  MQTT Connect / Reconnect  (NON-BLOCKING)
//  Called from loop(); never holds execution.
// ─────────────────────────────────────────────────────────────
static unsigned long mqttLastAttemptMs = 0;
const  unsigned long MQTT_RETRY_MS     = 20000;   // retry every 5 s

void tryMqttConnect() {
  if (mqtt.connected()) return;
  unsigned long now = millis();
  if (now - mqttLastAttemptMs < MQTT_RETRY_MS) return;
  mqttLastAttemptMs = now;

  Serial.print("[MQTT] Connecting…");
  if (mqtt.connect(MQTT_CLIENT)) {
    Serial.println(" Connected!");
    mqtt.subscribe(TOPIC_IRRIGATION);
    mqtt.subscribe(TOPIC_SPRAYER);
    mqtt.subscribe(TOPIC_GROWLIGHT);
    mqtt.subscribe(TOPIC_MODE);
  } else {
    Serial.printf(" Failed (rc=%d). Will retry in %lu s\n",
                  mqtt.state(), MQTT_RETRY_MS / 1000);
  }
}

// ─────────────────────────────────────────────────────────────
//  Sensor Simulation
// ─────────────────────────────────────────────────────────────

// Returns simulated hour-of-day (0-23.999) based on real elapsed time
float simulatedHour() {
  float elapsedSec = (float)(millis() / 1000UL);
  float fraction   = fmod(elapsedSec, SIMULATED_DAY_REAL_SECONDS) / SIMULATED_DAY_REAL_SECONDS;
  return fraction * 24.0f;
}

void updateMoisture() {
  static unsigned long lastMs = 0;
  unsigned long now = millis();
  if (lastMs == 0) { lastMs = now; return; }
  float dtSec = (now - lastMs) / 1000.0f;
  lastMs = now;

  if (irrigationOn) {
    moisture += 0.8f * dtSec;   // Rises quickly when watering
  } else {
    moisture -= 0.05f * dtSec;  // Slow evaporation
  }
  moisture = constrain(moisture, 0.0f, 100.0f);
}

void updateLight() {
  float h = simulatedHour();  // 0–24
  // Sine wave: peak at h=12 (noon), near-zero at night
  // sin(π * h/12 - π/2) maps 0→-1, 6→0, 12→+1, 18→0, 24→-1
  float raw = sinf((PI * h / 12.0f) - (PI / 2.0f));
  // Only daylight (raw > 0); at night use near-zero
  float normalised = max(0.0f, raw);
  // Add slight random variation ±5%
  float noise = ((float)random(-50, 50)) / 1000.0f;
  lightLux = (int)constrain((normalised + noise) * 1000.0f, 0.0f, 1000.0f);
}

void updatePest() {
  unsigned long now = millis();
  if (pestDetected) return;  // Already detected; automation will clear it
  if (now - lastPestMs < PEST_COOLDOWN_MS) return;

  // Random chance per call (~every 2 s)
  float roll = (float)random(0, 10000) / 10000.0f;
  if (roll < pestChancePerSec * 2.0f) {  // *2 because called every ~2 s
    pestDetected = true;
    lastPestMs   = now;
    Serial.println("[Pest] Detected!");
  }
}

// Push new moisture reading into ring buffer
void recordHistory() {
  moistureHistory[historyIndex] = moisture;
  historyIndex = (historyIndex + 1) % HISTORY_LEN;
  if (historyIndex == 0) historyFull = true;
}

// ─────────────────────────────────────────────────────────────
//  Automation Engine (AUTO mode only)
// ─────────────────────────────────────────────────────────────
void runAutomation() {
  if (!autoMode) return;

  unsigned long now = millis();

  // --- Irrigation: ON if moisture < 30%, run for 5 s ---
  if (!irrigationOn && moisture < 30.0f) {
    irrigationOn     = true;
    irrigationEndMs  = now + 5000;
    Serial.println("[Auto] Irrigation ON (dry soil)");
    mqtt.publish(TOPIC_IRRIGATION, "ON");
  }
  if (irrigationOn && irrigationEndMs > 0 && now >= irrigationEndMs) {
    irrigationOn    = false;
    irrigationEndMs = 0;
    Serial.println("[Auto] Irrigation OFF");
    mqtt.publish(TOPIC_IRRIGATION, "OFF");
  }

  // --- Sprayer: ON if pest detected, run for 3 s ---
  if (!sprayerOn && pestDetected) {
    sprayerOn     = true;
    sprayerEndMs  = now + 3000;
    Serial.println("[Auto] Sprayer ON (pest)");
    mqtt.publish(TOPIC_SPRAYER, "ON");
  }
  if (sprayerOn && sprayerEndMs > 0 && now >= sprayerEndMs) {
    sprayerOn      = false;
    sprayerEndMs   = 0;
    pestDetected   = false;   // Clear flag after spray
    Serial.println("[Auto] Sprayer OFF, pest cleared");
    mqtt.publish(TOPIC_SPRAYER, "OFF");
    mqtt.publish(TOPIC_PEST, "0");
  }

  // --- Grow light: ON if lux < 200, OFF otherwise ---
  bool wantLight = (lightLux < 200);
  if (wantLight != growlightOn) {
    growlightOn = wantLight;
    mqtt.publish(TOPIC_GROWLIGHT, growlightOn ? "ON" : "OFF");
    Serial.printf("[Auto] Grow light %s\n", growlightOn ? "ON" : "OFF");
  }
}

// ─────────────────────────────────────────────────────────────
//  JSON Status Builder 
// ─────────────────────────────────────────────────────────────
String buildStatusJson() {
  String j = "{";
  j += "\"moisture\":";   j += String(moisture, 1);      j += ",";
  j += "\"light\":";      j += String(lightLux);         j += ",";
  j += "\"pest\":\"";     j += (pestDetected ? "1" : "0"); j += "\",";
  j += "\"irrigation\":\""; j += (irrigationOn ? "ON" : "OFF"); j += "\",";
  j += "\"sprayer\":\"";    j += (sprayerOn    ? "ON" : "OFF"); j += "\",";
  j += "\"growlight\":\"";  j += (growlightOn  ? "ON" : "OFF"); j += "\",";
  j += "\"mode\":\"";       j += (autoMode ? "auto" : "manual"); j += "\",";

  // History array
  j += "\"history\":[";
  int len   = historyFull ? HISTORY_LEN : historyIndex;
  int start = historyFull ? historyIndex : 0;
  for (int i = 0; i < len; i++) {
    int idx = (start + i) % HISTORY_LEN;
    j += String(moistureHistory[idx], 1);
    if (i < len - 1) j += ",";
  }
  j += "]}";
  return j;
}

// ─────────────────────────────────────────────────────────────
//  Web Server Setup
// ─────────────────────────────────────────────────────────────
void setupWebServer() {

  // ── GET / → Dashboard ──────────────────────────────────────
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html", HTML_PAGE);
  });

  // ── GET /api/status → JSON ─────────────────────────────────
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    String json = buildStatusJson();
    req->send(200, "application/json", json);
  });

  // ── GET /api/control?device=...&state=... ──────────────────
  server.on("/api/control", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("device") || !req->hasParam("state")) {
      req->send(400, "text/plain", "Missing params");
      return;
    }
    String device = req->getParam("device")->value();
    String state  = req->getParam("state")->value();

    if (device == "mode") {
      autoMode = (state == "auto");
    } else if (!autoMode) {
      // Manual mode: allow direct control
      if (device == "irrigation") {
        irrigationOn = (state == "ON");
        mqtt.publish(TOPIC_IRRIGATION, irrigationOn ? "ON" : "OFF");
      } else if (device == "sprayer") {
        sprayerOn = (state == "ON");
        mqtt.publish(TOPIC_SPRAYER, sprayerOn ? "ON" : "OFF");
      } else if (device == "growlight") {
        growlightOn = (state == "ON");
        mqtt.publish(TOPIC_GROWLIGHT, growlightOn ? "ON" : "OFF");
      }
    }

    req->send(200, "text/plain", "OK");
  });

  // ── 404 fallback ───────────────────────────────────────────
  server.onNotFound([](AsyncWebServerRequest* req) {
    req->send(404, "text/plain", "Not found");
  });

  server.begin();
  Serial.println("[Web] Server started on port 80");
}

// ─────────────────────────────────────────────────────────────
//  MQTT Publish
// ─────────────────────────────────────────────────────────────
void publishSensors() {
  char buf[16];

  // Moisture (float → string)
  dtostrf(moisture, 1, 1, buf);
  mqtt.publish(TOPIC_MOISTURE, buf);

  // Light (int)
  itoa(lightLux, buf, 10);
  mqtt.publish(TOPIC_LIGHT, buf);

  // Pest
  mqtt.publish(TOPIC_PEST, pestDetected ? "1" : "0");

  Serial.printf("[MQTT] Pub — Moisture:%.1f%% Light:%d Pest:%d\n",
                moisture, lightLux, (int)pestDetected);
}

// ─────────────────────────────────────────────────────────────
//  setup()
// ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n╔══════════════════════════════╗");
  Serial.println("║  Virtual Smart Greenhouse    ║");
  Serial.println("╚══════════════════════════════╝");

  // Seed random generator from floating ADC pin
  randomSeed(analogRead(0) + micros());

  // Initialise history buffer to current moisture
  for (int i = 0; i < HISTORY_LEN; i++) moistureHistory[i] = moisture;

  setupWiFi();

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  // MQTT connection attempted non-blocking from loop()

  setupWebServer();

  Serial.printf("\n🌿 Dashboard: http://%s/\n\n", WiFi.localIP().toString().c_str());
}

// ─────────────────────────────────────────────────────────────
//  loop()
// ─────────────────────────────────────────────────────────────
void loop() {
  // ── MQTT keep-alive (non-blocking) ───────────────────────
  tryMqttConnect();
  mqtt.loop();

  // ── Sensor updates (every ~1 s) ───────────────────────────
  static unsigned long lastSensorMs = 0;
  unsigned long now = millis();
  if (now - lastSensorMs >= 1000) {
    lastSensorMs = now;
    updateMoisture();
    updateLight();
    updatePest();
  }

  // ── Automation engine ─────────────────────────────────────
  runAutomation();

  // ── Publish & record history ──────────────────────────────
  if (now - lastPublishMs >= PUBLISH_INTERVAL_MS) {
    lastPublishMs = now;
    publishSensors();
    recordHistory();
  }

  // Small yield to prevent watchdog issues
  yield();
}
