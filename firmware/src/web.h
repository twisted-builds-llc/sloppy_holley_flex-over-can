#pragma once
// WiFi AP web panel (structure from CANFLEXPDU Ver12 WebPanel.h).
// The web task only queues commands and serves the JSON the main loop publishes;
// the main loop owns outputs, CAN TX and NVS.
#include <WiFi.h>
#include <WebServer.h>
#include <freertos/queue.h>
#include <esp_random.h>
#include <math.h>

enum WebAction : uint8_t { WEB_STOP, WEB_AUTO, WEB_TEST, WEB_SET, WEB_HEARTBEAT, WEB_TX_START, WEB_TX_STOP,
                           WEB_AIN_TEST, WEB_SAVE_ROWS, WEB_SET_PASSWORD };
struct WebCommand { WebAction action; uint8_t index,value; uint8_t rows[4]; float volts; uint32_t at; char text[64]; };
QueueHandle_t webCommands=nullptr;
portMUX_TYPE webMux=portMUX_INITIALIZER_UNLOCKED;
char webState[1600]="{\"ready\":false}";
char webToken[17];
char webSsid[16];
WebServer panel(80);

bool parseWebNumber(const String &input,float &value){
  if(!input.length() || input.length()>16) return false;
  char *end=nullptr;value=strtof(input.c_str(),&end);
  return end!=input.c_str() && *end==0 && isfinite(value);
}
bool parseWebRow(const String &input,uint8_t &row){
  float v;if(!parseWebNumber(input,v) || v!=floorf(v) || v<1 || v>62) return false;
  row=uint8_t(v);return true;
}

const char WEB_PAGE[] PROGMEM=R"PAGE(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Flex-over-CAN</title>
<style>
:root{color-scheme:dark;--yellow:#ffdc00;--muted:#aaa99e;--red:#ffbdbd}*{box-sizing:border-box}body{margin:0;background:#090909;color:#f6f4e8;font:16px system-ui,sans-serif}main{max-width:1050px;margin:auto;padding:24px}header{border-bottom:2px solid var(--yellow);padding:10px 0 22px;display:flex;justify-content:space-between;align-items:center;gap:12px}h1{font-size:clamp(26px,5vw,42px);letter-spacing:-1px;margin:0;color:var(--yellow)}h2{margin:0 0 4px;color:var(--yellow);font-size:18px}.eyebrow{font-size:11px;letter-spacing:3px;color:var(--muted);margin-bottom:8px}.badge{border:1px solid #5c5410;color:var(--yellow);padding:8px 12px;border-radius:30px;font-size:12px}.row{display:flex;flex-wrap:wrap;gap:12px;margin:20px 0}.card{background:#141414;border:1px solid #32322b;border-radius:16px;padding:20px}.metric{flex:1;min-width:135px}.label{color:var(--muted);font-size:12px;text-transform:uppercase;letter-spacing:1px}.value{font-size:30px;margin:8px 0;color:var(--yellow);font-weight:700}.sub{font-size:13px;color:var(--muted);line-height:1.6}.buttons{display:flex;gap:10px;flex-wrap:wrap;margin:16px 0}button{border:1px solid #615600;border-radius:9px;background:#26230d;color:var(--yellow);padding:13px 18px;font:700 14px system-ui;cursor:pointer}button.primary{background:var(--yellow);color:#111}button.stop{background:#3b1717;border-color:#a93a3a;color:var(--red)}button:disabled{opacity:.35;cursor:not-allowed}.grid{display:grid;grid-template-columns:repeat(4,1fr);gap:14px}.out .lamp{width:12px;height:12px;display:inline-block;background:#595950;border-radius:50%;margin-right:8px}.out.on{border-color:var(--yellow)}.out.on .lamp{background:var(--yellow);box-shadow:0 0 15px #ffdc0070}.out button{width:100%;margin-top:14px}.notice{border-left:3px solid var(--yellow);padding:12px 16px;background:#201e0c;line-height:1.6;margin:18px 0}details{margin-top:18px}summary{cursor:pointer;color:var(--yellow);padding:6px 0}.diag{white-space:pre-wrap;font:13px ui-monospace,monospace;line-height:1.8;color:var(--muted)}input{margin-top:8px;padding:12px;width:130px;background:#080808;color:var(--yellow);border:1px solid #615600;border-radius:8px;font:inherit}label{color:var(--muted);font-size:13px}#error{color:var(--red)}@media(max-width:750px){.grid{grid-template-columns:repeat(2,1fr)}main{padding:16px}}@media(max-width:400px){.grid{grid-template-columns:1fr}}
</style></head><body><main>
<header><div><div class="eyebrow">HOLLEY CAN MODULE</div><h1>FLEX-OVER-CAN</h1></div><span class="badge" id="connection">CONNECTING</span></header>
<div class="row"><div class="card metric"><div class="label">Output mode</div><div class="value" id="mode">—</div><div class="sub" id="modeDetail">Waiting for board</div></div><div class="card metric"><div class="label">Holley RPM</div><div class="value" id="rpm">—</div><div class="sub" id="ecu">ECU —</div></div><div class="card metric"><div class="label">CAN bus</div><div class="value" id="can">—</div><div class="sub" id="frames">1 Mbps / extended</div></div><div class="card metric"><div class="label">To Holley (device 1985)</div><div class="value" id="tx">—</div><div class="sub" id="txDetail">—</div></div></div>
<section class="card"><div class="label">Output control</div><div class="buttons"><button class="primary control" onclick="command('auto')">HOLLEY AUTO</button><button class="control" onclick="armTest()">ARM 30s OUTPUT TEST</button><button class="stop control" onclick="command('stop')">ALL OFF / DISARM</button></div><div class="sub">AUTO follows the Holley rows below; stale or invalid commands turn an output OFF. TEST needs live CAN, engine stopped (RPM 0) and no Holley row ON. It ends after 30 s, 3 s without heartbeat, or when this tab is hidden.</div></section>
<div class="notice" id="notice">Connecting. No commands have been sent.</div><div id="error" role="alert"></div>
<section class="grid" id="outputs"></section>
<section class="card" style="margin-top:22px"><h2>Inputs sent to Holley</h2>
<div class="row"><div class="metric"><div class="label">Ethanol (Input1)</div><div class="value" id="eth">—</div><div class="sub" id="flexDetail">—</div></div><div class="metric"><div class="label">Fuel temp (Input2)</div><div class="value" id="temp">—</div><div class="sub">Sent as approximate GM CTS ohms</div></div></div>
<div class="row" id="ain"></div><div class="sub" id="ainNote">Analog inputs 3-6 are unverified on Holley until Bench A.</div></section>
<details><summary>Bench tools</summary><div class="card" style="margin-top:10px"><label>Fixed test volts on Input3-6<br><input id="ainTestV" type="number" min="0" max="5.5" step="0.01" value="2.5"></label><div class="buttons"><button class="control" onclick="ainTest()">SEND TEST VOLTS</button><button class="control" onclick="command('ainTest','&volts=off')">STOP TEST VOLTS</button><button class="control" onclick="command('txStart')">TX START</button><button class="stop control" onclick="command('txStop')">TX STOP</button></div><div class="sub">TX STOP silences device 1985 until TX START. Raw Holley output frames: serial command CAPTURE.</div></div></details>
<details><summary>Settings</summary><div class="card" style="margin-top:10px"><div class="label">Holley I/O row for each output (1-62)</div><div class="row" id="rowInputs"></div><div class="buttons"><button class="control" onclick="saveRows()">SAVE ROWS</button></div><div class="sub">Saving requires ALL OFF / DISARM first.</div>
<div class="label" style="margin-top:20px">WiFi password (8-63 characters)</div><label><input id="pass" type="password" minlength="8" maxlength="63" autocomplete="new-password" style="width:260px"></label><div class="buttons"><button class="control" onclick="setPass()">SAVE PASSWORD</button></div><div class="sub">Takes effect after the next power cycle. Serial command WIFI prints the current credentials.</div></div></details>
<details><summary>Diagnostics</summary><pre class="diag" id="diag">Waiting</pre></details>
<footer class="sub" style="margin:25px 0">Output state is the commanded state, not measured load current. Test with loads disconnected first. Never use for engine control before the validations in docs/PROGRESS.md.</footer>
</main><script>
const token='__TOKEN__';let state=null,online=false,busy=false,lastGood=0,rowsFilled=false;
const $=id=>document.getElementById(id);
for(let i=0;i<4;i++){$('outputs').insertAdjacentHTML('beforeend',`<article class="card out" id="o${i}"><div class="label" id="ol${i}">OUTPUT ${i+1}</div><h2><span class="lamp"></span>OUT${i+1}</h2><div class="value" id="ov${i}">—</div><div class="sub" id="os${i}">Waiting</div><button id="ob${i}" disabled onclick="toggle(${i})">TEST ON</button></article>`);
$('ain').insertAdjacentHTML('beforeend',`<div class="metric"><div class="label">Analog ${i+1} (Input${i+3})</div><div class="value" id="a${i}">—</div></div>`);
$('rowInputs').insertAdjacentHTML('beforeend',`<label>OUT${i+1}<br><input id="row${i}" type="number" min="1" max="62" step="1"></label>`);}
async function send(action,extra=''){let r=await fetch('/api/control',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded','X-Control-Token':token},body:'action='+action+extra,signal:AbortSignal.timeout(1500)});if(!r.ok)throw Error(await r.text());}
async function command(action,extra=''){if(!online||busy)return;busy=true;try{await send(action,extra);$('error').textContent='';}catch(e){$('error').textContent='Command not confirmed: '+e.message;}finally{busy=false;}}
function armTest(){if(confirm('Loads disconnected and engine stopped? This allows manual output commands for up to 30 seconds.'))command('test');}
function toggle(i){if(state?.mode==='TEST')command('set','&out='+i+'&on='+((state.mask&(1<<i))?0:1));}
function ainTest(){const v=Number($('ainTestV').value);if(!(v>=0&&v<=5.5)){$('error').textContent='Enter 0-5.5 V.';return;}command('ainTest','&volts='+encodeURIComponent(v));}
function saveRows(){let q='';for(let i=0;i<4;i++){const v=Number($('row'+i).value);if(!Number.isInteger(v)||v<1||v>62){$('error').textContent='Rows must be whole numbers 1-62.';return;}q+='&r'+i+'='+v;}command('rows',q);}
function setPass(){const p=$('pass').value;if(p.length<8||p.length>63){$('error').textContent='Password must be 8-63 characters.';return;}if(confirm('Save new WiFi password? You will need it after the next power cycle.')){command('password','&pass='+encodeURIComponent(p));$('pass').value='';}}
const fmt=(v,d)=>v===null?'—':v.toFixed(d);
function render(s){state=s;online=true;lastGood=Date.now();$('connection').textContent='CONNECTED';
$('mode').textContent=s.mode;$('modeDetail').textContent=s.mode==='TEST'?s.remaining+' s remaining':s.mode==='AUTO'?'Following Holley rows':'All outputs OFF';
$('rpm').textContent=s.rpmLive?Math.round(s.rpm):'—';$('ecu').textContent=s.ecu?'ECU serial 0x'+s.ecu.toString(16).toUpperCase():'ECU not seen yet';
$('can').textContent=s.canLive?'LIVE':'NOT READY';$('frames').textContent=s.frames+' frames / '+s.errors+' bus errors';
$('tx').textContent=s.tx;$('txDetail').textContent=s.txFrames+' frames sent'+(s.tx==='CONFLICT'?' / another device 1985 seen: remove it and reset':'');
$('notice').textContent=s.notice;
for(let i=0;i<4;i++){const on=!!(s.mask&(1<<i));$('o'+i).classList.toggle('on',on);$('ol'+i).textContent='OUTPUT '+(i+1)+' / ROW '+s.rows[i];$('ov'+i).textContent=on?'ON':'OFF';$('os'+i).textContent='Holley: '+s.outputs[i];$('ob'+i).textContent=on?'TEST OFF':'TEST ON';$('ob'+i).disabled=s.mode!=='TEST';
$('a'+i).textContent=fmt(s.ain[i],3)+(s.ain[i]===null?'':' V');if(!rowsFilled)$('row'+i).value=s.rows[i];}rowsFilled=true;
document.querySelectorAll('.control').forEach(b=>b.disabled=false);
$('eth').textContent=s.flex==='OK'?s.ethanol.toFixed(1)+' %':'—';$('flexDetail').textContent=s.flex==='OK'?s.flexHz.toFixed(2)+' Hz (provisional)':'Sensor: '+s.flex;$('temp').textContent=s.flex==='OK'?s.tempF.toFixed(1)+' °F':'—';
$('ainNote').textContent=(s.ainTest!==null?'TEST VOLTS '+s.ainTest.toFixed(2)+' V being sent on Input3-6. ':s.adc?'':'ADS1115 not found. ')+'Analog inputs 3-6 are unverified on Holley until Bench A.';
$('diag').textContent=`WiFi: ${s.ssid} / ${s.clients} client(s)\nCAN frames: ${s.frames} / bus errors: ${s.errors} / RX missed: ${s.missed}\nDevice 1985 TX: ${s.tx}, ${s.txFrames} frames\nUptime: ${Math.floor(s.uptime/1000)} s`;}
function disconnected(){online=false;$('connection').textContent='CONNECTION LOST';$('modeDetail').textContent='State may be stale. A manual test stops after 3 s without heartbeat.';document.querySelectorAll('button').forEach(b=>b.disabled=true);for(let i=0;i<4;i++)$('ov'+i).textContent='—';}
async function poll(){try{const r=await fetch('/api/status',{cache:'no-store',signal:AbortSignal.timeout(1500)});if(!r.ok)throw Error();const s=await r.json();if(s.ready===false)throw Error();render(s);}catch(e){disconnected();}setTimeout(poll,500);}
setInterval(()=>{if(online&&state?.mode==='TEST'&&!document.hidden)send('heartbeat').catch(()=>{});if(lastGood&&Date.now()-lastGood>2000)disconnected();},800);
document.addEventListener('visibilitychange',()=>{if(document.hidden&&state?.mode==='TEST')send('stop').catch(()=>{});});poll();
</script></body></html>)PAGE";

void webWorker(void*){
  const char *keys[]={"X-Control-Token"};panel.collectHeaders(keys,1);
  panel.on("/",HTTP_GET,[]{
    String page=FPSTR(WEB_PAGE);page.replace("__TOKEN__",webToken);
    panel.sendHeader("Cache-Control","no-store");panel.sendHeader("X-Frame-Options","DENY");
    panel.send(200,"text/html",page);
  });
  panel.on("/api/status",HTTP_GET,[]{
    char copy[sizeof(webState)];portENTER_CRITICAL(&webMux);memcpy(copy,webState,sizeof(copy));portEXIT_CRITICAL(&webMux);
    panel.sendHeader("Cache-Control","no-store");panel.send(200,"application/json",copy);
  });
  panel.on("/api/control",HTTP_POST,[]{
    if(panel.header("X-Control-Token")!=webToken){panel.send(403,"text/plain","Reload the page to get the current control token.");return;}
    WebCommand c={};c.at=millis();String action=panel.arg("action");
    if(action=="stop") c.action=WEB_STOP;
    else if(action=="auto") c.action=WEB_AUTO;
    else if(action=="test") c.action=WEB_TEST;
    else if(action=="heartbeat") c.action=WEB_HEARTBEAT;
    else if(action=="txStart") c.action=WEB_TX_START;
    else if(action=="txStop") c.action=WEB_TX_STOP;
    else if(action=="set"){
      String i=panel.arg("out"),v=panel.arg("on");
      if(i.length()!=1 || i[0]<'0' || i[0]>'3' || (v!="0" && v!="1")){panel.send(400,"text/plain","Invalid output command");return;}
      c.action=WEB_SET;c.index=i[0]-'0';c.value=v=="1";
    }else if(action=="ainTest"){
      c.action=WEB_AIN_TEST;
      if(panel.arg("volts")=="off") c.volts=NAN;
      else if(!parseWebNumber(panel.arg("volts"),c.volts) || c.volts<0 || c.volts>5.5f){panel.send(400,"text/plain","Test volts must be 0-5.5");return;}
    }else if(action=="rows"){
      c.action=WEB_SAVE_ROWS;
      for(unsigned i=0;i<4;++i) if(!parseWebRow(panel.arg(String("r")+i),c.rows[i])){panel.send(400,"text/plain","Rows must be 1-62");return;}
    }else if(action=="password"){
      String pass=panel.arg("pass");
      if(pass.length()<8 || pass.length()>63){panel.send(400,"text/plain","Password must be 8-63 characters");return;}
      c.action=WEB_SET_PASSWORD;strlcpy(c.text,pass.c_str(),sizeof(c.text));
    }else{panel.send(400,"text/plain","Unknown action");return;}
    // STOP jumps the queue so it overrides anything already waiting.
    BaseType_t ok=c.action==WEB_STOP?xQueueSendToFront(webCommands,&c,0):xQueueSend(webCommands,&c,0);
    panel.send(ok==pdTRUE?202:503,"text/plain",ok==pdTRUE?"Queued; check live status":"Busy; retry");
  });
  panel.onNotFound([]{panel.send(404,"text/plain","Open http://192.168.4.1");});
  panel.begin();
  for(;;){panel.handleClient();vTaskDelay(2);}
}

// Radio must already be on (WiFi.mode) when settings generate a first-boot password.
bool webStart(const char *password){
  snprintf(webToken,sizeof(webToken),"%08lX%08lX",(unsigned long)esp_random(),(unsigned long)esp_random());
  uint8_t mac[6];WiFi.softAPmacAddress(mac);
  snprintf(webSsid,sizeof(webSsid),"FLEXCAN-%02X%02X",mac[4],mac[5]);
  webCommands=xQueueCreate(12,sizeof(WebCommand));
  if(!webCommands || !WiFi.softAP(webSsid,password,6,false,2)) return false;
  return xTaskCreate(webWorker,"WEB",8192,nullptr,1,nullptr)==pdPASS;
}
