// webserver.cpp — Web vmesnik za fancontrol
// Single-page app: Dashboard / Ventilator / Kalibracija / Sistem
// WiFi statični IP, NTP, mDNS, OTA flash

#include "webserver.h"
#include "globals.h"
#include "config.h"
#include "logging.h"
#include "graph_store.h"
#include "fan.h"
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Update.h>
#include <ESPmDNS.h>
#include <ezTime.h>
#include "wifi_config.h"

static AsyncWebServer server(80);

// =====================================================================
// WiFi — statični IP
// =====================================================================
static void connectWiFi() {
    WiFi.mode(WIFI_STA);
    IPAddress ip, gw, sn, dns;
    ip.fromString(STATIC_IP);
    gw.fromString(STATIC_GW);
    sn.fromString(STATIC_SUBNET);
    dns.fromString(STATIC_DNS);
    WiFi.config(ip, gw, sn, dns);
    const char* ssid = (strlen(settings.ssid) > 0) ? settings.ssid : WIFI_SSID;
    const char* pass = (strlen(settings.password) > 0) ? settings.password : WIFI_PASSWORD;
    WiFi.begin(ssid, pass);
    LOG_INFO("WIFI", "Connecting: ssid=%s source=%s",
             ssid, (strlen(settings.ssid) > 0) ? "NVS" : "wifi_config.h");
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
        delay(300);
    }
    if (WiFi.status() == WL_CONNECTED) {
        LOG_INFO("WIFI", "Connected. IP: %s RSSI: %d dBm",
                 WiFi.localIP().toString().c_str(), WiFi.RSSI());
    } else {
        LOG_WARN("WIFI", "Connect failed — brez omrezja");
        sensorData.err |= ERR_WIFI;
    }
}

// =====================================================================
// NTP sinhronizacija
// =====================================================================
static void syncNTP() {
    if (WiFi.status() != WL_CONNECTED) return;
    // Čisti ezTime pristop — brez mešanja s configTime()
    setServer("pool.ntp.org");
    myTZ.setLocation(F("Europe/Ljubljana"));
    waitForSync(10);
    if (timeStatus() != timeNotSet) {
        timeSynced = true;
        lastNtpSyncMs = millis();
        LOG_INFO("NTP", "Synced: %s", myTZ.dateTime("d.m.Y H:i:s").c_str());
    } else {
        LOG_WARN("NTP", "Sync timeout — brez NTP");
        sensorData.err |= ERR_NTP;
    }
}

// =====================================================================
// OTA HTML — identično vent_SEW, prilagojeno za fancontrol
// =====================================================================
static const char OTA_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="sl"><head><meta charset="UTF-8">
<title>fancontrol OTA Update</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
*{box-sizing:border-box}
body{font-family:Arial,sans-serif;background:#0d0d0d;color:#e0e0e0;display:flex;flex-direction:column;align-items:center;padding:40px 20px}
h1{color:#00d4ff;margin-bottom:8px}
.sub{color:#888;font-size:14px;margin-bottom:30px}
.card{background:#1a1a1a;border:1px solid #2a2a2a;border-radius:10px;padding:30px 36px;width:100%;max-width:480px;text-align:center}
input[type=file]{display:block;width:100%;padding:10px;margin:16px 0 20px;background:#2a2a2a;border:2px dashed #555;border-radius:6px;color:#e0e0e0;cursor:pointer}
input[type=file]:hover{border-color:#00d4ff}
.btn{display:inline-block;padding:12px 32px;background:#00d4ff;color:#0d0d0d;border:none;border-radius:6px;font-size:16px;font-weight:bold;cursor:pointer;width:100%}
.btn:hover{background:#33dfff}
.btn:disabled{background:#555;color:#888;cursor:not-allowed}
#progress{width:100%;background:#2a2a2a;border-radius:4px;height:18px;margin-top:18px;display:none;overflow:hidden}
#bar{height:100%;background:#00d4ff;width:0;transition:width 0.3s;border-radius:4px}
#status{margin-top:14px;font-size:14px;color:#00d4ff;min-height:20px}
.nav{margin-top:28px;font-size:14px}.nav a{color:#00d4ff;text-decoration:none}
</style></head><body>
<h1>&#11014; OTA Firmware Update</h1>
<p class="sub">fancontrol — ESP32-S3</p>
<div class="card">
<form id="upForm">
<input type="file" id="file" accept=".bin" required>
<button class="btn" id="btn" type="submit">Nalozi firmware</button>
</form>
<div id="progress"><div id="bar"></div></div>
<div id="status"></div>
</div>
<div class="nav"><a href="/">&#8592; Nazaj</a></div>
<script>
document.getElementById('upForm').onsubmit=function(e){
  e.preventDefault();
  const f=document.getElementById('file').files[0];if(!f)return;
  const btn=document.getElementById('btn'),bar=document.getElementById('bar'),
        prog=document.getElementById('progress'),status=document.getElementById('status');
  btn.disabled=true;prog.style.display='block';status.textContent='Nalaganje...';
  const xhr=new XMLHttpRequest();
  xhr.upload.onprogress=function(e){if(e.lengthComputable){const p=Math.round(e.loaded/e.total*100);bar.style.width=p+'%';status.textContent='Nalaganje: '+p+'%';}};
  xhr.onload=function(){if(xhr.status===200){bar.style.width='100%';bar.style.background='#30d158';status.textContent='Uspelo! Naprava se resetira v 5s...';setTimeout(()=>{location.href='/';},5500);}else{bar.style.background='#ff3b30';status.textContent='Napaka: '+xhr.responseText;btn.disabled=false;}};
  xhr.onerror=function(){status.textContent='Napaka pri prenosu!';btn.disabled=false;};
  const form=new FormData();form.append('update',f);
  xhr.open('POST','/update');xhr.send(form);
};
</script></body></html>
)rawliteral";

// =====================================================================
// Glavna SPA stran — dark tehno tema, 4 tabi
// =====================================================================
static const char MAIN_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="sl"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>FANCONTROL</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#0d0d0d;color:#e0e0e0;font-family:monospace,system-ui;min-height:100vh}
header{background:#1a1a1a;border-bottom:2px solid #00d4ff;padding:10px 20px;display:flex;justify-content:space-between;align-items:center}
h1{color:#00d4ff;font-size:18px;letter-spacing:4px}
.hr{font-size:11px;color:#888;text-align:right;line-height:1.7}
.hr span{color:#00d4ff}
nav{background:#1a1a1a;border-bottom:1px solid #2a2a2a;display:flex}
.t{padding:11px 18px;cursor:pointer;font-size:12px;color:#555;border-bottom:2px solid transparent;letter-spacing:1px;transition:color .2s}
.t.on{color:#00d4ff;border-bottom-color:#00d4ff}
main{max-width:920px;margin:0 auto;padding:16px}
.pane{display:none}.pane.on{display:block}
.cards{display:flex;flex-wrap:wrap;gap:10px;margin-bottom:14px}
.card{background:#1a1a1a;border:1px solid #2a2a2a;border-radius:8px;padding:12px 16px;flex:1;min-width:130px}
.ctit{font-size:10px;color:#555;text-transform:uppercase;letter-spacing:1px;margin-bottom:3px}
.cval{font-size:28px;color:#00d4ff;font-weight:bold}
.cunit{font-size:12px;color:#555}
.cpeak{font-size:10px;color:#ff9500;margin-top:3px}
.cdnd{font-size:10px;color:#ff9500;margin-top:3px}
.cw{background:#1a1a1a;border:1px solid #2a2a2a;border-radius:8px;padding:12px;margin-bottom:10px}
.ct2{font-size:10px;color:#555;margin-bottom:6px;letter-spacing:1px;text-transform:uppercase}
.sec{background:#1a1a1a;border:1px solid #2a2a2a;border-radius:8px;padding:16px;margin-bottom:12px}
.sec h3{color:#00d4ff;font-size:13px;letter-spacing:2px;margin-bottom:12px;padding-bottom:8px;border-bottom:1px solid #2a2a2a;text-transform:uppercase}
.fr{display:flex;align-items:center;gap:10px;margin-bottom:8px}
.fr label{font-size:12px;color:#aaa;min-width:170px}
input[type=text],input[type=number],input[type=password]{background:#0d0d0d;border:1px solid #333;color:#e0e0e0;padding:5px 9px;border-radius:4px;font-family:monospace;font-size:12px;width:120px}
input[type=checkbox]{width:16px;height:16px;accent-color:#00d4ff}
input:focus{outline:none;border-color:#00d4ff}
.btn{padding:9px 22px;background:#00d4ff;color:#0d0d0d;border:none;border-radius:6px;cursor:pointer;font-weight:bold;font-family:monospace;font-size:12px;letter-spacing:1px}
.btn:hover{background:#33dfff}
.bto{background:#ff9500;color:#0d0d0d}.bto:hover{background:#ffb733}
.bsm{padding:5px 12px;font-size:11px}
.msg{margin-left:10px;font-size:11px}
table{width:100%;border-collapse:collapse;font-size:11px}
th{color:#555;text-align:left;padding:5px 7px;border-bottom:1px solid #2a2a2a;font-size:10px;letter-spacing:1px;text-transform:uppercase}
td{padding:4px 7px;border-bottom:1px solid #161616}
.li{color:#888}.lw{color:#ff9500}.le{color:#ff3b30}
.ir{display:flex;justify-content:space-between;padding:7px 0;border-bottom:1px solid #1e1e1e;font-size:12px}
.ik{color:#888}.iv{color:#00d4ff;font-weight:bold}
.eb{display:inline-block;padding:2px 9px;border-radius:4px;font-size:11px}
.eok{background:#0a2a0a;color:#30d158}.efail{background:#2a0a0a;color:#ff3b30}
.ctbl td,th{min-width:60px}
</style></head><body>
<header>
<div><h1>&#9650; FANCONTROL</h1></div>
<div class="hr">
<div>IP: <span id="hip">...</span></div>
<div>Uptime: <span id="hup">...</span></div>
<div><span id="htim">--:--:--</span></div>
</div>
</header>
<nav>
<div class="t on" onclick="sw(0)">DASHBOARD</div>
<div class="t" onclick="sw(1)">VENTILATOR</div>
<div class="t" onclick="sw(2)">KALIBRACIJA</div>
<div class="t" onclick="sw(3)">SISTEM</div>
</nav>
<main>
<!-- TAB 0: DASHBOARD -->
<div class="pane on" id="p0">
<div class="cards">
<div class="card"><div class="ctit">Temperatura</div><div><span class="cval" id="ct">--</span><span class="cunit"> °C</span></div><div class="cpeak" id="pkt"></div></div>
<div class="card"><div class="ctit">Vlažnost</div><div><span class="cval" id="ch">--</span><span class="cunit"> %</span></div></div>
<div class="card"><div class="ctit">Napetost</div><div><span class="cval" id="cv">--</span><span class="cunit"> V</span></div></div>
<div class="card"><div class="ctit">Poraba</div><div><span class="cval" id="cw">--</span><span class="cunit"> W</span></div><div class="cpeak" id="pkw"></div></div>
<div class="card"><div class="ctit">Ventilator</div><div><span class="cval" id="cf">--</span><span class="cunit"> %</span></div><div class="cdnd" id="cdnd"></div></div>
</div>
<div class="cw"><div class="ct2">Temperatura / Vlažnost</div><canvas id="gTH" height="90"></canvas></div>
<div class="cw"><div class="ct2">Ventilator %</div><canvas id="gFan" height="60"></canvas></div>
<div class="cw"><div class="ct2">Poraba [W]</div><canvas id="gW" height="60"></canvas></div>
</div>
<!-- TAB 1: VENTILATOR -->
<div class="pane" id="p1">
<div class="sec"><h3>Temperaturna krivulja</h3>
<table class="ctbl" style="margin-bottom:14px"><tr><th>Točka</th><th>Temp [°C]</th><th>Fan [%]</th></tr>
<tr><td>1</td><td><input type="number" id="ct0" min="0" max="80" step="0.5" style="width:80px"></td><td><input type="number" id="cp0" min="0" max="100" style="width:70px"></td></tr>
<tr><td>2</td><td><input type="number" id="ct1" min="0" max="80" step="0.5" style="width:80px"></td><td><input type="number" id="cp1" min="0" max="100" style="width:70px"></td></tr>
<tr><td>3</td><td><input type="number" id="ct2" min="0" max="80" step="0.5" style="width:80px"></td><td><input type="number" id="cp2" min="0" max="100" style="width:70px"></td></tr>
<tr><td>4</td><td><input type="number" id="ct3" min="0" max="80" step="0.5" style="width:80px"></td><td><input type="number" id="cp3" min="0" max="100" style="width:70px"></td></tr>
</table></div>
<div class="sec"><h3>Limiti in DND</h3>
<div class="fr"><label>Min hitrost [%]</label><input type="number" id="fMin" min="0" max="100" style="width:70px"></div>
<div class="fr"><label>Max podnevi [%]</label><input type="number" id="fMaxD" min="0" max="100" style="width:70px"></div>
<div class="fr"><label>Max DND [%]</label><input type="number" id="fDndM" min="0" max="100" style="width:70px"></div>
<div class="fr"><label>DND omogočen</label><input type="checkbox" id="dndE"></div>
<div class="fr"><label>DND od ure (0–23)</label><input type="number" id="dndF" min="0" max="23" style="width:70px"></div>
<div class="fr"><label>DND do ure (0–23)</label><input type="number" id="dndT" min="0" max="23" style="width:70px"></div>
<button class="btn" onclick="saveFan()">Shrani</button><span class="msg" id="msgF"></span>
</div></div>
<!-- TAB 2: KALIBRACIJA -->
<div class="pane" id="p2">
<div class="sec"><h3>SHT30 Kalibracija</h3>
<div class="fr"><label>Temp offset [°C]</label><input type="number" id="tOff" step="0.1"></div>
<div class="fr"><label>Hum offset [%]</label><input type="number" id="hOff" step="0.1"></div>
</div>
<div class="sec"><h3>INA219 Kalibracija</h3>
<div class="fr"><label>Shunt [Ω]</label><input type="number" id="sOhm" step="0.001" min="0.001"></div>
<div class="fr"><label>Korekcija toka</label><input type="number" id="cCorr" step="0.01" min="0.01"></div>
</div>
<div class="sec"><h3>WiFi</h3>
<div class="fr"><label>SSID</label><input type="text" id="wSsid" maxlength="31" style="width:200px"></div>
<div class="fr"><label>Geslo</label><input type="password" id="wPass" maxlength="63" style="width:200px"></div>
<button class="btn" onclick="saveCal()">Shrani</button><span class="msg" id="msgC"></span>
</div></div>
<!-- TAB 3: SISTEM -->
<div class="pane" id="p3">
<div class="sec"><h3>Sistemske informacije</h3><div id="sysinfo"></div></div>
<div class="sec">
<h3>RAM Log &nbsp;<button class="btn bsm" onclick="fetchLog()">Osveži</button>&nbsp;<button class="btn bsm bto" onclick="clearLog()">Počisti</button></h3>
<div style="overflow-x:auto;margin-top:10px">
<table id="logtbl"><tr><th>Čas</th><th>Level</th><th>Tag</th><th>Sporočilo</th></tr></table>
</div></div>
<div class="sec"><h3>OTA Firmware Update</h3>
<form id="otaF">
<input type="file" id="otaBin" accept=".bin" required style="display:block;width:100%;padding:8px;margin:8px 0 12px;background:#0d0d0d;border:2px dashed #333;border-radius:6px;color:#e0e0e0;cursor:pointer">
<button class="btn" type="submit">Naloži firmware</button>
</form>
<div id="otaPrg" style="display:none;width:100%;background:#2a2a2a;border-radius:4px;height:12px;margin-top:10px;overflow:hidden">
<div id="otaBar" style="height:100%;background:#00d4ff;width:0;transition:width 0.3s;border-radius:4px"></div></div>
<div id="otaSt" style="margin-top:8px;font-size:12px;color:#00d4ff"></div>
</div></div>
</main>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js"></script>
<script>
// Tab switching
let atab=0,_fL=false,_cL=false;
function sw(i){
  document.querySelectorAll('.t').forEach((e,n)=>e.classList.toggle('on',n===i));
  document.querySelectorAll('.pane').forEach((e,n)=>e.classList.toggle('on',n===i));
  atab=i;
  if(i===1&&!_fL)loadFan();
  if(i===2&&!_cL)loadCal();
  if(i===3){fetchSys();fetchLog();}
}

// Charts
Chart.defaults.color='#555';Chart.defaults.borderColor='#222';
function mkChart(id,ds,sc){
  return new Chart(document.getElementById(id),{
    type:'line',data:{labels:[],datasets:ds},
    options:{responsive:true,animation:false,
      plugins:{legend:{position:'top',labels:{boxWidth:10,font:{size:10}}}},
      scales:sc}
  });
}
const gTH=mkChart('gTH',[
  {label:'Temp °C',data:[],borderColor:'#ff3b30',backgroundColor:'transparent',yAxisID:'y',tension:0.3,pointRadius:2},
  {label:'Vlaga %',data:[],borderColor:'#555',backgroundColor:'transparent',yAxisID:'y2',tension:0.3,pointRadius:2}
],{y:{title:{display:true,text:'°C',color:'#555'}},
   y2:{position:'right',title:{display:true,text:'%',color:'#555'},grid:{drawOnChartArea:false}}});
const gFan=mkChart('gFan',[
  {label:'Fan %',data:[],borderColor:'#00d4ff',backgroundColor:'rgba(0,212,255,0.07)',tension:0.3,pointRadius:2,fill:true}
],{y:{min:0,max:100}});
const gW=mkChart('gW',[
  {label:'Watt',data:[],borderColor:'#30d158',backgroundColor:'rgba(48,209,88,0.07)',tension:0.3,pointRadius:2,fill:true}
],{y:{min:0}});

function fUp(s){const h=Math.floor(s/3600),m=Math.floor((s%3600)/60),sec=s%60;return(h?h+'h ':'')+m+'m '+sec+'s';}

// Osveži kartice in grafe iz /api/data in /api/history
async function refreshData(){
  try{
    const d=await(await fetch('/api/data')).json();
    document.getElementById('hip').textContent=d.ip;
    document.getElementById('hup').textContent=fUp(d.uptime);
    document.getElementById('htim').textContent=d.time;
    document.getElementById('ct').textContent=d.temp.toFixed(1);
    document.getElementById('ch').textContent=d.hum.toFixed(1);
    document.getElementById('cv').textContent=d.volt.toFixed(2);
    document.getElementById('cw').textContent=d.watt.toFixed(1);
    document.getElementById('cf').textContent=d.fan;
    document.getElementById('cdnd').textContent=d.dnd?'DND aktiven':'';
    document.getElementById('pkt').textContent=d.peak_temp>-900?'Peak: '+d.peak_temp.toFixed(1)+' °C':'';
    document.getElementById('pkw').textContent=d.peak_watt>0?'Peak: '+d.peak_watt.toFixed(1)+' W':'';
    if(atab===0)loadHistory();
  }catch(e){}
}

async function loadHistory(){
  try{
    const pts=await(await fetch('/api/history')).json();
    const lb=pts.map(p=>{const d=new Date(p.ts*1000);return String(d.getHours()).padStart(2,'0')+':'+String(d.getMinutes()).padStart(2,'0');});
    gTH.data.labels=gFan.data.labels=gW.data.labels=lb;
    gTH.data.datasets[0].data=pts.map(p=>p.temp);
    gTH.data.datasets[1].data=pts.map(p=>p.hum);
    gTH.update('none');
    gFan.data.datasets[0].data=pts.map(p=>p.fan);gFan.update('none');
    gW.data.datasets[0].data=pts.map(p=>p.watt);gW.update('none');
  }catch(e){}
}

setInterval(refreshData,5000);
refreshData();

// Naloži nastavitve ventilatorja za Tab 1
async function loadFan(){
  try{
    const s=await(await fetch('/api/fansettings')).json();
    for(let i=0;i<4;i++){document.getElementById('ct'+i).value=s.curveTemp[i];document.getElementById('cp'+i).value=s.curvePct[i];}
    document.getElementById('fMin').value=s.fanMinPct;
    document.getElementById('fMaxD').value=s.fanMaxDayPct;
    document.getElementById('fDndM').value=s.dndMaxPct;
    document.getElementById('dndE').checked=s.dndEnabled;
    document.getElementById('dndF').value=s.dndFrom;
    document.getElementById('dndT').value=s.dndTo;
    _fL=true;
  }catch(e){}
}

// Naloži kalibracijske nastavitve za Tab 2
async function loadCal(){
  try{
    const s=await(await fetch('/api/calsettings')).json();
    document.getElementById('tOff').value=s.tempOffset;
    document.getElementById('hOff').value=s.humOffset;
    document.getElementById('sOhm').value=s.shuntOhms;
    document.getElementById('cCorr').value=s.currentCorr;
    document.getElementById('wSsid').value=s.ssid;
    _cL=true;
  }catch(e){}
}

// Shrani fan nastavitve
async function saveFan(){
  const ct=[0,1,2,3].map(i=>parseFloat(document.getElementById('ct'+i).value)||0);
  const cp=[0,1,2,3].map(i=>parseInt(document.getElementById('cp'+i).value)||0);
  const b={curveTemp:ct,curvePct:cp,
    fanMinPct:parseInt(document.getElementById('fMin').value)||0,
    fanMaxDayPct:parseInt(document.getElementById('fMaxD').value)||100,
    dndMaxPct:parseInt(document.getElementById('fDndM').value)||30,
    dndEnabled:document.getElementById('dndE').checked,
    dndFrom:parseInt(document.getElementById('dndF').value)||22,
    dndTo:parseInt(document.getElementById('dndT').value)||7};
  const r=await fetch('/save/fan',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});
  const m=document.getElementById('msgF');
  m.textContent=r.ok?'Shranjeno!':'Napaka!';m.style.color=r.ok?'#30d158':'#ff3b30';
  setTimeout(()=>m.textContent='',3000);
}

// Shrani kalibracijo + WiFi
async function saveCal(){
  const b={
    tempOffset:parseFloat(document.getElementById('tOff').value)||0,
    humOffset:parseFloat(document.getElementById('hOff').value)||0,
    shuntOhms:parseFloat(document.getElementById('sOhm').value)||0.1,
    currentCorr:parseFloat(document.getElementById('cCorr').value)||1,
    ssid:document.getElementById('wSsid').value,
    password:document.getElementById('wPass').value};
  const r=await fetch('/save/cal',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});
  const m=document.getElementById('msgC');
  m.textContent=r.ok?'Shranjeno! WiFi se posodobi ob resetu.':'Napaka!';m.style.color=r.ok?'#30d158':'#ff3b30';
  setTimeout(()=>m.textContent='',5000);
}

// Sistemske informacije (Tab 3)
async function fetchSys(){
  try{
    const d=await(await fetch('/api/data')).json();
    document.getElementById('sysinfo').innerHTML=
      `<div class="ir"><span class="ik">IP</span><span class="iv">${d.ip}</span></div>`+
      `<div class="ir"><span class="ik">RSSI</span><span class="iv">${d.rssi} dBm</span></div>`+
      `<div class="ir"><span class="ik">Uptime</span><span class="iv">${fUp(d.uptime)}</span></div>`+
      `<div class="ir"><span class="ik">Firmware</span><span class="iv">${d.fw}</span></div>`+
      `<div class="ir"><span class="ik">Free heap</span><span class="iv">${d.heap} B</span></div>`+
      `<div class="ir"><span class="ik">Napake</span><span class="iv"><span class="eb ${d.err===0?'eok':'efail'}">${d.err===0?'OK':'ERR 0x'+d.err.toString(16).toUpperCase()}</span></span></div>`;
  }catch(e){}
}

// RAM Log
async function fetchLog(){
  try{
    const logs=await(await fetch('/api/log')).json();
    const t=document.getElementById('logtbl');
    t.innerHTML='<tr><th>Čas</th><th>Level</th><th>Tag</th><th>Sporočilo</th></tr>';
    logs.forEach(e=>{
      const cls=e.level==='ERROR'?'le':e.level==='WARN'?'lw':'li';
      const tr=document.createElement('tr');tr.className=cls;
      tr.innerHTML=`<td>${e.time}</td><td>${e.level}</td><td>${e.tag}</td><td>${e.msg}</td>`;
      t.appendChild(tr);
    });
  }catch(e){}
}

async function clearLog(){
  try{await fetch('/api/log/clear',{method:'POST'});fetchLog();}catch(e){}
}

// OTA upload
document.getElementById('otaF').onsubmit=function(e){
  e.preventDefault();
  const f=document.getElementById('otaBin').files[0];if(!f)return;
  const btn=this.querySelector('button[type=submit]'),
        bar=document.getElementById('otaBar'),
        prg=document.getElementById('otaPrg'),
        st=document.getElementById('otaSt');
  btn.disabled=true;prg.style.display='block';st.textContent='Nalaganje...';
  const xhr=new XMLHttpRequest();
  xhr.upload.onprogress=function(ev){if(ev.lengthComputable){const p=Math.round(ev.loaded/ev.total*100);bar.style.width=p+'%';st.textContent='Nalaganje: '+p+'%';}};
  xhr.onload=function(){if(xhr.status===200){bar.style.width='100%';bar.style.background='#30d158';st.textContent='Uspelo! Naprava se resetira v 5s...';setTimeout(()=>{location.href='/';},5500);}else{bar.style.background='#ff3b30';st.textContent='Napaka: '+xhr.responseText;btn.disabled=false;}};
  xhr.onerror=function(){st.textContent='Napaka pri prenosu!';btn.disabled=false;};
  const fd=new FormData();fd.append('update',f);
  xhr.open('POST','/update');xhr.send(fd);
};
</script></body></html>
)rawliteral";

// =====================================================================
// Pomožna: oblikuj čas kot HH:MM:SS
// =====================================================================
static void getTimeStr(char* buf, size_t sz) {
    if (timeSynced) {
        snprintf(buf, sz, "%02d:%02d:%02d", myTZ.hour(), myTZ.minute(), myTZ.second());
    } else {
        unsigned long s = millis() / 1000;
        snprintf(buf, sz, "%02lu:%02lu:%02lu", (s / 3600) % 24, (s / 60) % 60, s % 60);
    }
}

// =====================================================================
// initWebserver — WiFi, NTP, mDNS, registracija endpointov
// =====================================================================
void initWebserver() {
    connectWiFi();
    if (WiFi.status() == WL_CONNECTED) {
        syncNTP();
        MDNS.begin(MDNS_HOSTNAME);
        LOG_INFO("MDNS", "http://%s.local", MDNS_HOSTNAME);
    }

    // --- GET / → glavna SPA stran ---
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send_P(200, "text/html; charset=UTF-8", MAIN_HTML);
    });

    // --- GET /api/data → JSON trenutnih vrednosti ---
    server.on("/api/data", HTTP_GET, [](AsyncWebServerRequest *request){
        StaticJsonDocument<512> doc;
        portENTER_CRITICAL(&dataMux);
        float temp = sensorData.temp, hum = sensorData.hum;
        float volt = sensorData.volt, amp = sensorData.amp, watt = sensorData.watt;
        uint8_t fan = sensorData.fanPct;
        bool dnd = sensorData.dndActive;
        uint8_t err = sensorData.err;
        portEXIT_CRITICAL(&dataMux);

        doc["temp"]      = serialized(String(temp, 1));
        doc["hum"]       = serialized(String(hum, 1));
        doc["volt"]      = serialized(String(volt, 2));
        doc["amp"]       = serialized(String(amp, 3));
        doc["watt"]      = serialized(String(watt, 1));
        doc["fan"]       = fan;
        doc["dnd"]       = dnd;
        doc["peak_temp"] = serialized(String(peakTemp, 1));
        doc["peak_watt"] = serialized(String(peakWatt, 1));

        char tbuf[10];
        getTimeStr(tbuf, sizeof(tbuf));
        doc["time"]   = tbuf;
        doc["uptime"] = millis() / 1000;
        doc["rssi"]   = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
        doc["ip"]     = STATIC_IP;
        doc["err"]    = err;
        doc["fw"]     = FW_VERSION;
        doc["heap"]   = ESP.getFreeHeap();

        String resp;
        serializeJson(doc, resp);
        request->send(200, "application/json", resp);
    });

    // --- GET /api/history → JSON array za grafe ---
    server.on("/api/history", HTTP_GET, [](AsyncWebServerRequest *request){
        int cnt = graphGetCount();
        String json = "[";
        for (int i = 0; i < cnt; i++) {
            GraphPoint p = graphGetPoint(i);
            if (i > 0) json += ",";
            char buf[96];
            snprintf(buf, sizeof(buf),
                     "{\"ts\":%lu,\"temp\":%.1f,\"hum\":%.0f,\"watt\":%.1f,\"fan\":%u}",
                     (unsigned long)p.ts, p.temp, p.hum, p.watt, (unsigned)p.fanPct);
            json += buf;
        }
        json += "]";
        request->send(200, "application/json", json);
    });

    // --- GET /api/log → JSON array zadnjih 50 vnosov ---
    server.on("/api/log", HTTP_GET, [](AsyncWebServerRequest *request){
        int count = logGetCount();
        int start = (count > 50) ? count - 50 : 0;
        DynamicJsonDocument doc(10240);
        JsonArray arr = doc.to<JsonArray>();
        for (int i = start; i < count; i++) {
            LogEntry e = logGetEntry(i);
            JsonObject o = arr.createNestedObject();
            o["time"] = e.time;
            o["level"] = (e.level == LOG_LVL_ERROR) ? "ERROR" :
                         (e.level == LOG_LVL_WARN)  ? "WARN"  : "INFO";
            o["tag"]  = e.tag;
            o["msg"]  = e.msg;
        }
        String resp;
        serializeJson(doc, resp);
        request->send(200, "application/json", resp);
    });

    // --- POST /api/log/clear → počisti log buffer ---
    server.on("/api/log/clear", HTTP_POST, [](AsyncWebServerRequest *request){
        logClear();
        LOG_INFO("WEB", "/api/log/clear");
        request->send(200, "application/json", "{\"status\":\"cleared\"}");
    });

    // --- GET /api/fansettings → za pre-fill Tab 1 ---
    server.on("/api/fansettings", HTTP_GET, [](AsyncWebServerRequest *request){
        StaticJsonDocument<512> doc;
        JsonArray ct = doc.createNestedArray("curveTemp");
        JsonArray cp = doc.createNestedArray("curvePct");
        for (int i = 0; i < FAN_CURVE_POINTS; i++) {
            ct.add(settings.curveTemp[i]);
            cp.add(settings.curvePct[i]);
        }
        doc["fanMinPct"]    = settings.fanMinPct;
        doc["fanMaxDayPct"] = settings.fanMaxDayPct;
        doc["dndMaxPct"]    = settings.dndMaxPct;
        doc["dndEnabled"]   = settings.dndEnabled;
        doc["dndFrom"]      = settings.dndFrom;
        doc["dndTo"]        = settings.dndTo;
        String resp;
        serializeJson(doc, resp);
        request->send(200, "application/json", resp);
    });

    // --- GET /api/calsettings → za pre-fill Tab 2 ---
    server.on("/api/calsettings", HTTP_GET, [](AsyncWebServerRequest *request){
        StaticJsonDocument<256> doc;
        doc["tempOffset"]  = settings.tempOffset;
        doc["humOffset"]   = settings.humOffset;
        doc["shuntOhms"]   = settings.shuntOhms;
        doc["currentCorr"] = settings.currentCorr;
        doc["ssid"]        = settings.ssid;
        String resp;
        serializeJson(doc, resp);
        request->send(200, "application/json", resp);
    });

    // --- POST /save/fan → shrani fan nastavitve v NVS ---
    server.on("/save/fan", HTTP_POST,
        [](AsyncWebServerRequest *request){},
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len,
           size_t index, size_t total){
            static String body;
            if (index == 0) body = "";
            for (size_t i = 0; i < len; i++) body += (char)data[i];
            if (index + len != total) return;

            StaticJsonDocument<512> doc;
            DeserializationError err = deserializeJson(doc, body);
            if (err) {
                LOG_ERROR("WEB", "/save/fan JSON error: %s", err.c_str());
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }

            // Validacija in vpis krivulje
            JsonArray ct = doc["curveTemp"];
            JsonArray cp = doc["curvePct"];
            if (ct.size() == FAN_CURVE_POINTS && cp.size() == FAN_CURVE_POINTS) {
                for (int i = 0; i < FAN_CURVE_POINTS; i++) {
                    float t = ct[i];
                    uint8_t p = cp[i];
                    if (t >= 0.0f && t <= 80.0f) settings.curveTemp[i] = t;
                    if (p <= 100) settings.curvePct[i] = p;
                }
            }
            if (doc.containsKey("fanMinPct")) {
                uint8_t v = doc["fanMinPct"];
                if (v <= 100) settings.fanMinPct = v;
            }
            if (doc.containsKey("fanMaxDayPct")) {
                uint8_t v = doc["fanMaxDayPct"];
                if (v <= 100) settings.fanMaxDayPct = v;
            }
            if (doc.containsKey("dndMaxPct")) {
                uint8_t v = doc["dndMaxPct"];
                if (v <= 100) settings.dndMaxPct = v;
            }
            if (doc.containsKey("dndEnabled")) settings.dndEnabled = doc["dndEnabled"];
            if (doc.containsKey("dndFrom")) {
                uint8_t v = doc["dndFrom"];
                if (v <= 23) settings.dndFrom = v;
            }
            if (doc.containsKey("dndTo")) {
                uint8_t v = doc["dndTo"];
                if (v <= 23) settings.dndTo = v;
            }

            saveSettings();
            LOG_INFO("WEB", "/save/fan OK");
            request->send(200, "application/json", "{\"status\":\"OK\"}");
        }
    );

    // --- POST /save/cal → shrani kalibracija + WiFi v NVS ---
    server.on("/save/cal", HTTP_POST,
        [](AsyncWebServerRequest *request){},
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len,
           size_t index, size_t total){
            static String body;
            if (index == 0) body = "";
            for (size_t i = 0; i < len; i++) body += (char)data[i];
            if (index + len != total) return;

            StaticJsonDocument<512> doc;
            DeserializationError err = deserializeJson(doc, body);
            if (err) {
                LOG_ERROR("WEB", "/save/cal JSON error: %s", err.c_str());
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }

            if (doc.containsKey("tempOffset"))  settings.tempOffset  = doc["tempOffset"];
            if (doc.containsKey("humOffset"))    settings.humOffset   = doc["humOffset"];
            if (doc.containsKey("shuntOhms"))    settings.shuntOhms   = doc["shuntOhms"];
            if (doc.containsKey("currentCorr"))  settings.currentCorr = doc["currentCorr"];
            if (doc.containsKey("ssid")) {
                const char* v = doc["ssid"];
                if (v) strncpy(settings.ssid, v, sizeof(settings.ssid) - 1);
            }
            if (doc.containsKey("password")) {
                const char* v = doc["password"];
                if (v) strncpy(settings.password, v, sizeof(settings.password) - 1);
            }

            saveSettings();
            // WiFi sprememba velja ob resetu — ne reconnectamo takoj
            LOG_INFO("WEB", "/save/cal OK (WiFi posodobitev ob resetu)");
            request->send(200, "application/json", "{\"status\":\"OK\"}");
        }
    );

    // --- GET /update → OTA HTML ---
    server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send_P(200, "text/html; charset=UTF-8", OTA_HTML);
    });

    // --- POST /update → OTA flash (identično vent_SEW) ---
    server.on("/update", HTTP_POST,
        [](AsyncWebServerRequest *request){
            bool ok = !Update.hasError();
            String msg = ok ? "OK" : Update.errorString();
            AsyncWebServerResponse *resp = request->beginResponse(
                ok ? 200 : 500, "text/plain",
                ok ? "OK" : ("FAIL: " + msg));
            resp->addHeader("Connection", "close");
            request->send(resp);
            if (ok) { delay(500); ESP.restart(); }
        },
        [](AsyncWebServerRequest *request, String filename,
           size_t index, uint8_t *data, size_t len, bool final){
            if (!index) {
                LOG_INFO("OTA", "Start: %s (%u B)",
                         filename.c_str(), request->contentLength());
                if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH))
                    LOG_ERROR("OTA", "begin failed: %s", Update.errorString());
            }
            if (!Update.hasError() && Update.write(data, len) != len)
                LOG_ERROR("OTA", "write failed");
            if (final) {
                if (Update.end(true))
                    LOG_INFO("OTA", "OK: %u B", index + len);
                else
                    LOG_ERROR("OTA", "end failed: %s", Update.errorString());
            }
        }
    );

    server.begin();
    LOG_INFO("WEB", "Server started on %s", STATIC_IP);
}

// =====================================================================
// handleWebserver — mDNS + NTP re-sync + WiFi watchdog (klic v loop)
// =====================================================================
void handleWebserver() {
    // ESP32 ESPmDNS teče v ozadju — update() ni potreben

    // NTP re-sync vsakih 30 min
    if (timeSynced && millis() - lastNtpSyncMs >= NTP_UPDATE_INTERVAL) {
        syncNTP();
    }

    // WiFi watchdog vsakih 10 min
    if (millis() - lastWifiCheckMs >= WIFI_CHECK_INTERVAL) {
        lastWifiCheckMs = millis();
        if (WiFi.status() != WL_CONNECTED) {
            LOG_WARN("WIFI", "Reconnecting...");
            connectWiFi();
            if (WiFi.status() == WL_CONNECTED && !timeSynced) syncNTP();
        }
    }
}
