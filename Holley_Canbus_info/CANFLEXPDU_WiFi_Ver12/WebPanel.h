#pragma once
#include <WiFi.h>
#include <WebServer.h>
#include <freertos/queue.h>
#include <esp_system.h>
#include <math.h>

constexpr char WEB_SSID[]="CANFLEX-PDU";
constexpr char WEB_PASSWORD[]="CanFlex-Test11"; // Change before sharing outside the bench.
enum WebAction : uint8_t { WEB_STOP, WEB_AUTO, WEB_ARM_TEST, WEB_SET, WEB_HEARTBEAT, WEB_FAUX_START, WEB_FAUX_STOP };
struct WebCommand { WebAction action; uint8_t index,value; uint32_t at; float percent,tempF; bool sweep; };
QueueHandle_t webCommands=nullptr;
portMUX_TYPE webMux=portMUX_INITIALIZER_UNLOCKED;
char webState[3200]="{\"ready\":false}";
char webToken[25];
bool webReady=false;
WebServer panel(80);

bool parseWebNumber(const String &input,float &value){
  if(!input.length() || input.length()>16)return false;
  char *end=nullptr; value=strtof(input.c_str(),&end);
  return end!=input.c_str() && *end==0 && isfinite(value);
}
const char WEB_PAGE[] PROGMEM=R"PAGE(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>CANFLEX PDU</title>
<style>
:root{color-scheme:dark;--yellow:#ffdc00;--muted:#aaa99e}*{box-sizing:border-box}body{margin:0;background:#090909;color:#f6f4e8;font:16px system-ui,sans-serif}main{max-width:1050px;margin:auto;padding:24px}header{border-bottom:2px solid var(--yellow);padding:10px 0 22px;display:flex;justify-content:space-between;align-items:center;gap:12px}h1{font-size:clamp(26px,5vw,42px);letter-spacing:-1px;margin:0;color:var(--yellow)}.eyebrow{font-size:11px;letter-spacing:3px;color:var(--muted);margin-bottom:8px}.badge{border:1px solid #5c5410;color:var(--yellow);padding:8px 12px;border-radius:30px;font-size:12px}.statusbar{display:flex;flex-wrap:wrap;gap:12px;margin:20px 0}.card{background:#141414;border:1px solid #32322b;border-radius:16px;padding:20px}.metric{flex:1;min-width:135px}.label{color:var(--muted);font-size:12px;text-transform:uppercase;letter-spacing:1px}.value{font-size:30px;margin:8px 0;color:var(--yellow);font-weight:700}.sub{font-size:13px;color:var(--muted);line-height:1.6}.controls{margin:20px 0}.buttons{display:flex;gap:10px;flex-wrap:wrap;margin:16px 0}button{border:1px solid #615600;border-radius:9px;background:#26230d;color:var(--yellow);padding:13px 18px;font:700 14px system-ui;cursor:pointer}button.primary{background:var(--yellow);color:#111}button.stop{background:#3b1717;border-color:#a93a3a;color:#ffbdbd}button:disabled{opacity:.35;cursor:not-allowed}.grid{display:grid;grid-template-columns:repeat(4,1fr);gap:14px}.relay h2{font-size:17px;margin:12px 0}.lamp{width:12px;height:12px;display:inline-block;background:#595950;border-radius:50%;margin-right:8px}.relay.on{border-color:var(--yellow)}.relay.on .lamp{background:var(--yellow);box-shadow:0 0 15px #ffdc0070}.relay .value{font-size:26px}.relay button{width:100%;margin-top:14px}.notice{border-left:3px solid var(--yellow);padding:12px 16px;background:#201e0c;line-height:1.6;margin:18px 0}details{margin-top:22px}summary{cursor:pointer;color:var(--yellow)}.diag{white-space:pre-wrap;font:13px ui-monospace,monospace;line-height:1.8;color:var(--muted)}footer{margin:25px 0;color:var(--muted);font-size:12px;line-height:1.8}@media(max-width:750px){.grid{grid-template-columns:repeat(2,1fr)}main{padding:16px}}@media(max-width:400px){.grid{grid-template-columns:1fr}}#error{color:#ffbdbd}a{color:var(--yellow)}
</style></head><body><main>
<header><div><div class="eyebrow">WIRELESS CONTROL / VER12</div><h1>CANFLEX PDU<span style="color:#777"> / 04</span></h1></div><span class="badge" id="connection">CONNECTING</span></header>
<div class="statusbar"><div class="card metric"><div class="label">Control mode</div><div class="value" id="mode">—</div><div class="sub" id="modeDetail">Waiting for board</div></div><div class="card metric"><div class="label">Holley RPM</div><div class="value" id="rpm">—</div><div class="sub">Live ECU telemetry</div></div><div class="card metric"><div class="label">CAN bus</div><div class="value" id="can">—</div><div class="sub" id="rate">1 Mbps / extended CAN</div></div></div>
<section class="card controls"><div class="label">Choose control</div><div class="buttons"><button class="primary control" onclick="command('auto')">HOLLEY AUTO</button><button class="control" onclick="armTest()">ARM 30s RELAY TEST</button><button class="stop control" onclick="command('stop')">ALL OFF / DISARM</button></div><div class="sub">AUTO follows Holley rows 5–8. Manual TEST temporarily overrides AUTO and requires fresh stopped-engine RPM, healthy CAN, and all four Holley commands OFF. Test ends after 30 seconds or 3 seconds without browser heartbeat.</div></section>
<div class="notice" id="notice">Connecting. No commands have been sent.</div><div id="error" role="alert"></div>
<section class="grid" id="relays"></section>
<section class="card" style="margin-top:22px"><div class="eyebrow">FLEX / TEMPERATURE</div><h2 style="margin:0;color:var(--yellow)">Slim monitor &amp; faux bench source</h2>
<div class="statusbar"><div class="metric"><div class="label">External device 1985</div><div class="value" id="flexLive">—</div><div class="sub" id="flexDetail">Waiting for Slim frames</div></div><div class="metric"><div class="label">External temperature</div><div class="value" id="tempLive">—</div><div class="sub">Reconstructed from CAN resistance / approximate GM curve</div></div><div class="metric"><div class="label">PDU faux output</div><div class="value" id="fauxState">OFF</div><div class="sub" id="fauxDetail">Synthetic source disabled by default</div></div></div>
<div class="notice" id="fauxNotice">Keep Slim connected for live monitoring. To broadcast faux values, disconnect Slim, reset PDU, and keep the engine off.</div>
<div style="display:flex;gap:18px;flex-wrap:wrap"><label>Ethanol %<br><input id="fauxPercent" type="number" min="0" max="100" step="0.1" value="30" style="margin-top:8px;padding:12px;width:130px;background:#080808;color:#ffdc00;border:1px solid #615600;border-radius:8px"></label><label>Fuel temperature °F<br><input id="fauxTemp" type="number" min="-40" max="257" step="0.1" value="75" style="margin-top:8px;padding:12px;width:130px;background:#080808;color:#ffdc00;border:1px solid #615600;border-radius:8px"></label><label>Test pattern<br><select id="fauxPattern" style="margin-top:8px;padding:12px;background:#080808;color:#ffdc00;border:1px solid #615600;border-radius:8px"><option value="0">Hold chosen values</option><option value="1">Sweep 20–40% / 68–140°F</option></select></label></div>
<div class="buttons"><button id="fauxArm" class="control" onclick="startFaux()">ARM 60s FAUX BROADCAST</button><button class="stop control" onclick="command('fauxStop')">STOP FAUX / ALL OFF</button></div>
<div class="sub">Bench test only. Starting faux mode opens relay outputs and inhibits AUTO. It uses device 1985 and cannot run alongside the Slim or genuine module. Requires stopped-engine RPM, healthy CAN, all Holley outputs OFF, and browser heartbeat. No saved overrides. Reset always disables faux mode.</div></section>
<details><summary>Diagnostics &amp; network</summary><pre class="diag" id="diag">Waiting</pre><div class="sub">Wi-Fi: CANFLEX-PDU · Password: CanFlex-Test11<br>Open <a href="http://192.168.4.1">http://192.168.4.1</a>. Local network only; no internet or cloud required.</div></details>
<footer>Relay state shows GPIO command, not measured contact or load feedback. First test with loads disconnected.<br>Live Slim values and synthetic PDU values are shown separately. Faux values are never sensor measurements.<br>MAP lockout is not implemented in this version.</footer>
</main><script>
const token='__TOKEN__';let state=null,online=false,busy=false,lastGood=0;
const $=id=>document.getElementById(id);
for(let i=0;i<4;i++)$('relays').insertAdjacentHTML('beforeend',`<article class="card relay" id="r${i}"><div class="label">OUTPUT 0${i+1} / ROW ${i+5}</div><h2><span class="lamp"></span>Relay ${i+1}</h2><div class="value" id="v${i}">—</div><div class="sub" id="s${i}">Waiting</div><button id="b${i}" disabled onclick="toggle(${i})">TEST ON</button></article>`);
async function send(action,extra=''){let r=await fetch('/api/control',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded','X-Control-Token':token},body:'action='+action+extra,signal:AbortSignal.timeout(1500)});if(!r.ok)throw Error(await r.text());}
async function command(action,extra=''){if(!online||busy)return;busy=true;try{await send(action,extra);$('error').textContent='';}catch(e){$('error').textContent='Command not confirmed: '+e.message;}finally{busy=false;}}
function armTest(){if(confirm('Loads disconnected and engine stopped? This enables manual relay commands for up to 30 seconds.'))command('test');}
function startFaux(){const p=Number($('fauxPercent').value),t=Number($('fauxTemp').value);if(!Number.isFinite(p)||p<0||p>100||!Number.isFinite(t)||t< -40||t>257){$('error').textContent='Enter ethanol 0–100% and temperature -40–257°F.';return;}if(confirm('ENGINE OFF, loads disconnected, Slim / genuine 1985 removed? This broadcasts SYNTHETIC values to Holley for up to 60 seconds.'))command('fauxStart','&percent='+encodeURIComponent(p)+'&tempF='+encodeURIComponent(t)+'&sweep='+$('fauxPattern').value);}
function toggle(i){if(state?.mode==='TEST')command('set','&relay='+i+'&on='+((state.mask&(1<<i))?0:1));}
function render(s){state=s;online=true;lastGood=Date.now();$('connection').textContent='CONNECTED';$('mode').textContent=s.mode;$('modeDetail').textContent=s.mode==='TEST'?s.remaining+' seconds remaining':s.mode==='FAUX'?'Synthetic input test / relay outputs OFF':s.mode==='AUTO'?'Following Holley commands':s.autoPending?'Waiting for all four Holley commands OFF':'Outputs inhibited';$('rpm').textContent=s.rpmLive?Math.round(s.rpm):'—';$('can').textContent=s.canLive?'LIVE':'NOT READY';$('rate').textContent=s.fps.toFixed(0)+' frames / second';$('notice').textContent=s.notice;
for(let i=0;i<4;i++){let on=!!(s.mask&(1<<i));$('r'+i).classList.toggle('on',on);$('v'+i).textContent=on?'ON':'OFF';$('s'+i).textContent='Holley: '+s.outputs[i];$('b'+i).textContent=on?'TEST OFF':'TEST ON';$('b'+i).disabled=s.mode!=='TEST';}document.querySelectorAll('.control').forEach(b=>b.disabled=false);
$('flexLive').textContent=s.flexLive?s.ethanol.toFixed(1)+' %':'—';$('flexDetail').textContent=s.flexLive?s.flexHz.toFixed(2)+' Hz / received from external device':s.flexSeen?'Signal stale or invalid':'No external flex frame received';$('tempLive').textContent=s.tempLive?s.liveTempF.toFixed(1)+' °F':'—';$('fauxState').textContent=s.fauxActive?'SYNTHETIC':'OFF';$('fauxDetail').textContent=s.fauxActive?s.fauxPercent.toFixed(1)+'% / '+s.fauxTempF.toFixed(1)+'°F / '+s.fauxRemaining+'s left':'No PDU input broadcasts';$('fauxNotice').textContent=s.fauxBlocked?'Device 1985 detected: faux broadcasting blocked until it is disconnected and PDU reset. Live monitoring remains available.':s.fauxActive?'SYNTHETIC BROADCAST ACTIVE. Engine off; relays inhibited.':'Faux source available for unloaded engine-off bench test only. Disconnect Slim first.';
$('diag').textContent=`Application CAN TX: ${s.fauxActive?'FAUX TEST ONLY':'OFF'}\nDevice identity: 1985 (exclusive)\nFaux frames queued: ${s.fauxQueued}\nFrames: ${s.frames} / IDs: ${s.ids}\nBus errors: ${s.errors} / RX missed: ${s.missed} / overrun: ${s.overrun}\nUptime: ${Math.floor(s.uptime/1000)} seconds\nStation count: ${s.clients}\nFirmware: CANFLEXPDU_WiFi_Ver12`;}
function disconnected(){online=false;$('connection').textContent='CONNECTION LOST';$('modeDetail').textContent='State may be stale. Manual / faux testing stops after heartbeat timeout.';document.querySelectorAll('button').forEach(b=>b.disabled=true);for(let i=0;i<4;i++)$('v'+i).textContent='—';$('flexLive').textContent='—';$('tempLive').textContent='—';$('fauxState').textContent='UNKNOWN';$('fauxDetail').textContent='Connection lost; last state is stale.';}
async function poll(){try{const r=await fetch('/api/status',{cache:'no-store',signal:AbortSignal.timeout(1500)});if(!r.ok)throw Error();const s=await r.json();if(s.ready===false)throw Error();render(s);}catch(e){disconnected();}setTimeout(poll,500);}
setInterval(()=>{if(online&&(state?.mode==='TEST'||state?.fauxActive)&&!document.hidden)send('heartbeat').catch(()=>{});if(lastGood&&Date.now()-lastGood>2000)disconnected();},800);
document.addEventListener('visibilitychange',()=>{if(document.hidden&&(state?.mode==='TEST'||state?.fauxActive))send('stop').catch(()=>{});});poll();
</script></body></html>)PAGE";

void webWorker(void*) {
  const char* keys[]={"X-Control-Token"}; panel.collectHeaders(keys,1);
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
    WebCommand c={}; c.at=millis();String action=panel.arg("action");
    if(action=="stop")c.action=WEB_STOP;
    else if(action=="auto")c.action=WEB_AUTO;
    else if(action=="test")c.action=WEB_ARM_TEST;
    else if(action=="heartbeat")c.action=WEB_HEARTBEAT;
    else if(action=="fauxStop")c.action=WEB_FAUX_STOP;
    else if(action=="fauxStart"){
      c.action=WEB_FAUX_START;
      if(!parseWebNumber(panel.arg("percent"),c.percent) || !parseWebNumber(panel.arg("tempF"),c.tempF) ||
         c.percent<0 || c.percent>100 || c.tempF< -40 || c.tempF>257 || (panel.arg("sweep")!="0" && panel.arg("sweep")!="1")){
        panel.send(400,"text/plain","Invalid faux range or pattern");return;
      }
      c.sweep=panel.arg("sweep")=="1";
    }
    else if(action=="set"){
      String i=panel.arg("relay"),v=panel.arg("on");
      if(i.length()!=1 || i[0]<'0' || i[0]>'3' || (v!="0"&&v!="1")){panel.send(400,"text/plain","Invalid relay command");return;}
      c.action=WEB_SET;c.index=i[0]-'0';c.value=v=="1";
    }else{panel.send(400,"text/plain","Unknown action");return;}
    BaseType_t ok=(c.action==WEB_STOP || c.action==WEB_FAUX_STOP)?xQueueSendToFront(webCommands,&c,0):xQueueSend(webCommands,&c,0);
    panel.send(ok==pdTRUE?202:503,"text/plain",ok==pdTRUE?"Queued; check live status":"Busy; retry");
  });
  panel.onNotFound([]{panel.send(404,"text/plain","Open http://192.168.4.1");});
  panel.begin();for(;;){panel.handleClient();vTaskDelay(2);}
}

void webStart() {
  snprintf(webToken,sizeof(webToken),"%08lX%08lX",(unsigned long)esp_random(),(unsigned long)esp_random());
  webCommands=xQueueCreate(12,sizeof(WebCommand));if(!webCommands)return;
  WiFi.mode(WIFI_AP);
  if(!WiFi.softAP(WEB_SSID,WEB_PASSWORD,6,false,2))return;
  webReady=xTaskCreatePinnedToCore(webWorker,"PDU_WEB",10000,nullptr,1,nullptr,1)==pdPASS;
}

