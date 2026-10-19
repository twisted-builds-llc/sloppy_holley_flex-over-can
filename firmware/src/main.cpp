/* Sloppy Holley Flex-over-CAN -- ESP32-C3, Arduino-ESP32 3.x (pioarduino, see platformio.ini).
   To Holley as CAN device 1985: Input1 flex Hz, Input2 fuel temp as GM CTS ohms, Input3-6 analog volts.
   From Holley: IO_OUTPUT rows (config.h) -> OUT1-4 low-side, on/off. PWM waits for Bench B capture.
   Protocol status: docs/CAN_PROTOCOL.md. Not for engine control before docs/PROGRESS.md validations.
   WiFi AP panel at http://192.168.4.1 (web.h). Per-board random password; serial WIFI prints it.
   Serial 115200, commands + Enter: STATUS, ARM, DISARM, START, STOP, CAPTURE, AINTEST <volts>|OFF, WIFI, HELP. */
#include <Arduino.h>
#include <Wire.h>
#include <driver/twai.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include "pins.h"
#include "config.h"
#include "holley.h"
#include "ads1115.h"
#include "settings.h"
#include "web.h"
#include <stdarg.h>
using namespace holley;

// ================= Flex sensor (ported from CAN_Flex_Slim_Ver5) =================
constexpr uint32_t FLEX_TIMEOUT_US=100000;
portMUX_TYPE pulseMux=portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t lastFall=0,edgeTime=0,lowWidth=0,periodUs=0,sampleLow=0,sampleTime=0,sampleCount=0;
volatile bool haveFall=false,haveLow=false,sawEdge=false;

void ARDUINO_ISR_ATTR flexEdge(){
  uint32_t now=(uint32_t)esp_timer_get_time();
  bool high=gpio_get_level((gpio_num_t)PIN_FLEX);
  portENTER_CRITICAL_ISR(&pulseMux);
  edgeTime=now;sawEdge=true;
  if(!high){
    // Pair previous completed LOW with its falling-to-falling period.
    if(haveFall && haveLow){periodUs=now-lastFall;sampleLow=lowWidth;sampleTime=now;sampleCount=sampleCount+1;}
    lastFall=now;haveFall=true;haveLow=false;
  }else if(haveFall){lowWidth=now-lastFall;haveLow=true;}
  portEXIT_CRITICAL_ISR(&pulseMux);
}

enum FlexState { NO_PULSE, INCOMPLETE, BAD_FREQUENCY, BAD_WIDTH, FLEX_OK };
const char *const flexNames[]={"NO_PULSE","INCOMPLETE_OR_STALE","FREQUENCY_RANGE","LOW_WIDTH_RANGE","OK"};
FlexState flexState=NO_PULSE;
float flexHz=0,flexLowMs=0;

void updateFlex(){
  uint32_t p,l,t,e,n;bool edges;
  portENTER_CRITICAL(&pulseMux);
  p=periodUs;l=sampleLow;t=sampleTime;e=edgeTime;n=sampleCount;edges=sawEdge;
  uint32_t now=(uint32_t)esp_timer_get_time();
  // Drop partial measurement after silence; don't bridge disconnected bursts.
  if(!edges || now-e>FLEX_TIMEOUT_US){haveFall=false;haveLow=false;}
  portEXIT_CRITICAL(&pulseMux);
  FlexState state;
  if(!edges || now-e>FLEX_TIMEOUT_US) state=NO_PULSE;
  else if(!n || now-t>FLEX_TIMEOUT_US || !p) state=INCOMPLETE;
  else{
    flexHz=1000000.0f/p;flexLowMs=l/1000.0f;
    state=!flexFrequencyOk(flexHz)?BAD_FREQUENCY:!flexWidthOk(l,p)?BAD_WIDTH:FLEX_OK;
  }
  if(state!=flexState) Serial.printf("[FLEX] %s\n",flexNames[state]);
  flexState=state;
}

// ================= CAN receive (RX task, from CANFLEXPDU Ver12) =================
struct RowState { uint32_t ms,bits,status; bool seen,valid,on; };
struct BusState {
  uint32_t total,lastMs,rpmMs,polls,ecu; // ecu = learned ECU serial (0 until first RPM frame).
  float rpm; uint8_t rpmStatus;
  bool rpmSeen,rpmValid,conflict;
  RowState rows[OUTPUT_ROWS];
};
struct RowChange { uint8_t row; uint32_t bits,status; };
portMUX_TYPE busMux=portMUX_INITIALIZER_UNLOCKED;
BusState bus={};
QueueHandle_t rowChanges=nullptr; // Raw IO_OUTPUT changes for the CAPTURE log (Bench B).

void collect(const twai_message_t &m){
  uint32_t now=millis(),id=m.identifier;
  bool data=m.extd && !m.rtr;
  RowChange change={};bool changed=false;
  portENTER_CRITICAL(&busMux);
  ++bus.total;bus.lastMs=now;
  if(data){
    if((id&0x7FF)==DEVICE_SERIAL) bus.conflict=true; // Another device 1985 on the bus.
    if((id&~0x7FFu)==RPM_BASE && !bus.ecu) bus.ecu=id&0x7FF;
    if(bus.ecu && id==rpmId(bus.ecu) && m.data_length_code==8){
      float rpm=bitsToFloat(getU32BE(m.data));uint8_t status=m.data[7];
      // OK, Stall, Synchronized, Syncing; never treat a fault-tagged value as valid.
      bool statusOK=status==0 || status==32 || status==33 || status==34;
      bus.rpmSeen=true;bus.rpmMs=now;bus.rpm=rpm;bus.rpmStatus=status;
      bus.rpmValid=isfinite(rpm) && rpm>=0 && rpm<=20000 && statusOK;
    }
    if(bus.ecu && id==pollId(bus.ecu) && m.data_length_code==0) ++bus.polls;
    unsigned row=bus.ecu?outputRow(id,bus.ecu):0;
    if(row){
      RowState &r=bus.rows[row-1];
      uint32_t bits=0,status=0xFFFFFFFF;
      if(m.data_length_code==8){bits=getU32BE(m.data);status=getU32BE(m.data+4);}
      changed=!r.seen || bits!=r.bits || status!=r.status;
      change={uint8_t(row),bits,status};
      r={now,bits,status,true,validOutput(bits,status),bits==0x3F800000 && status==43};
    }
  }
  portEXIT_CRITICAL(&busMux);
  if(changed && rowChanges) xQueueSend(rowChanges,&change,0);
}
void receiveTask(void*){
  for(;;){
    twai_message_t m;
    // Never wait for a frame; yield one OS tick after a bounded batch.
    for(int i=0;i<128 && twai_receive(&m,0)==ESP_OK;++i) collect(m);
    vTaskDelay(1);
  }
}
BusState snapshot(){BusState s;portENTER_CRITICAL(&busMux);s=bus;portEXIT_CRITICAL(&busMux);return s;}

// ================= CAN transmit: device 1985 (from CAN_Flex_Slim_Ver5) =================
// TWAI stays in NORMAL mode; the transceiver silent pin gates the wire. This avoids
// reinstalling the driver under the RX task and covers the C3 errata noted in Ver5.
bool canStarted=false,txActive=false,manualStop=false;
uint32_t quietSince=0,quietNeeded=LISTEN_MS,lastSend=0,lastAnnounce=0,txFailedBaseline=0,polledSeen=0;
uint32_t txFrames=0;
twai_status_info_t canStatus={};
float analogTest=NAN; // AINTEST override for Bench A: fixed volts on Input3-6.

void setSilent(bool silent){digitalWrite(PIN_CAN_SILENT,silent?HIGH:LOW);}
void haltTx(const char *reason){
  if(txActive) Serial.printf("[TX STOPPED] %s\n",reason);
  txActive=false;setSilent(true);
  if(canStarted) twai_clear_transmit_queue();
  quietSince=millis();quietNeeded=TX_RETRY_MS;
}
bool sendFrame(uint32_t id,uint8_t dlc,const uint8_t *bytes){
  twai_message_t m={};m.identifier=id;m.extd=1;m.data_length_code=dlc;memcpy(m.data,bytes,dlc);
  esp_err_t r=twai_transmit(&m,0);
  if(r!=ESP_OK){haltTx(esp_err_to_name(r));return false;}
  ++txFrames;return true;
}
bool sendFloat(uint32_t id,float value){uint8_t d[4];putFloatBE(value,d);return sendFrame(id,4,d);}
void announce(uint32_t now){const uint8_t d[]={ANNOUNCE_BYTE};if(sendFrame(ANNOUNCE_ID,1,d))lastAnnounce=now;}

bool canLive(uint32_t now,const BusState &s){
  return canStarted && canStatus.state==TWAI_STATE_RUNNING && s.total && now-s.lastMs<=500;
}
const char *txText(const BusState &s){return s.conflict?"CONFLICT":txActive?"ACTIVE":manualStop?"STOPPED":"WAIT";}

void txTick(uint32_t now,const BusState &s,const Ads1115 &adc){
  bool live=canLive(now,s);
  if(!txActive){
    // Start only after a silent listen window with live traffic and no competing device 1985.
    if(manualStop || s.conflict || !live || now-quietSince<quietNeeded) return;
    txFailedBaseline=canStatus.tx_failed_count;polledSeen=s.polls;
    setSilent(false);txActive=true;lastSend=now-TX_PERIOD_MS;
    Serial.println("[TX ACTIVE] device 1985 broadcasting");
    announce(now);
    return;
  }
  if(s.conflict){haltTx("another device 1985 (0x7C1) detected; reset after removing it");return;}
  if(!live || canStatus.tx_failed_count!=txFailedBaseline ||
     canStatus.tx_error_counter>=128 || canStatus.rx_error_counter>=128){haltTx("CAN fault or bus inactive");return;}
  if(canStatus.msgs_to_tx){if(now-lastSend>250)haltTx("CAN TX pending beyond 250ms");return;} // No stale queueing.
  if(s.polls!=polledSeen || now-lastAnnounce>=ANNOUNCE_MS){polledSeen=s.polls;announce(now);return;}
  if(now-lastSend<TX_PERIOD_MS) return;
  lastSend=now;
  // Invalid sensors are skipped, not faked: no Holley fault flag is known. Holley may hold the last value.
  if(flexState==FLEX_OK){
    if(!sendFloat(inputId(1),flexInputHz(flexHz))) return;
    if(!sendFloat(inputId(2),gmCtsOhms(flexTempC(flexLowMs)))) return;
  }
  for(unsigned i=0;i<Ads1115::CHANNELS;++i){
    bool test=!isnan(analogTest);
    if(!test && !adc.fresh(i,now)) continue;
    if(!sendFloat(inputId(3+i),test?analogTest:adc.volts(i)*ANALOG_SCALE)) return;
  }
}

void canHealthTick(){
  if(!canStarted || twai_get_status_info(&canStatus)!=ESP_OK) return;
  if(canStatus.state==TWAI_STATE_BUS_OFF){haltTx("CAN bus-off, recovering");twai_initiate_recovery();}
  else if(canStatus.state==TWAI_STATE_STOPPED) twai_start(); // Recovery finished.
}

// ================= Outputs (policy from CANFLEXPDU Ver12) =================
// AUTO: follow fresh, valid Holley commands; stale/invalid/missing -> OFF.
// DISARMED: all OFF until AUTO. TEST: web manual commands, 30 s max with heartbeat.
enum Mode : uint8_t { MODE_AUTO, MODE_DISARMED, MODE_TEST };
const char *const modeNames[]={"AUTO","DISARMED","TEST"};
Mode mode=MODE_AUTO;
Settings settings;
uint8_t outputMask=0,testMask=0;
uint32_t testSince=0,testHeartbeat=0;
char notice[120]="Ready. AUTO follows Holley rows.";

void setNotice(const char *text){strlcpy(notice,text,sizeof(notice));}
void setMode(Mode m,const char *text){mode=m;testMask=0;setNotice(text);}
bool holleyOn(uint32_t now,const BusState &s,unsigned i){
  const RowState &r=s.rows[settings.rows[i]-1];
  return freshOutput(r.seen,r.valid,now,r.ms,OUTPUT_TIMEOUT_MS) && r.on;
}
// Manual test needs live CAN, a stopped engine, and no Holley row commanding its output ON.
bool testSafe(uint32_t now,const BusState &s){
  if(!canLive(now,s) || !s.rpmSeen || !s.rpmValid || now-s.rpmMs>500 || s.rpm>0.1f) return false;
  for(unsigned i=0;i<4;++i) if(holleyOn(now,s,i)) return false;
  return true;
}

void applyOutputs(uint32_t now,const BusState &s){
  if(mode==MODE_TEST && (testExpired(now,testSince,testHeartbeat,TEST_MAX_MS) || !testSafe(now,s)))
    setMode(MODE_DISARMED,"Test ended: timer, heartbeat, CAN, RPM or Holley command. Outputs OFF; press AUTO to resume.");
  uint8_t wanted=0;
  if(now>=BOOT_GUARD_MS){
    if(mode==MODE_TEST) wanted=testMask;
    else if(mode==MODE_AUTO && s.ecu) for(unsigned i=0;i<4;++i) if(holleyOn(now,s,i)) wanted|=1u<<i;
  }
  // OFF transitions first; only the main loop writes output GPIOs.
  for(unsigned i=0;i<4;++i) if(!(wanted&(1u<<i))) gpio_set_level((gpio_num_t)PIN_OUT[i],0);
  for(unsigned i=0;i<4;++i) if(wanted&(1u<<i)) gpio_set_level((gpio_num_t)PIN_OUT[i],1);
  outputMask=wanted;
}

// ================= Serial =================
Ads1115 adc;
bool captureEnabled=false;
char command[32];unsigned commandUsed=0;bool commandOverflow=false;

const char *rowText(const RowState &r,uint32_t now){
  return !r.seen?"WAIT":now-r.ms>OUTPUT_TIMEOUT_MS?"STALE":!r.valid?"INVALID":r.on?"ON":"OFF";
}
void report(uint32_t now,const BusState &s){
  bool rpmLive=s.rpmSeen && s.rpmValid && now-s.rpmMs<=500;
  Serial.printf("CAN=%s ecu=%03lX frames=%lu TX=%s sent=%lu conflict=%u RPM=%s%.0f OUT=%X%s\n",
    !canStarted?"ERROR":canStatus.state!=TWAI_STATE_RUNNING?"FAULT":!s.total?"WAIT":now-s.lastMs<=500?"LIVE":"STALE",
    (unsigned long)s.ecu,(unsigned long)s.total,txActive?"ACTIVE":manualStop?"STOPPED":"WAIT",(unsigned long)txFrames,
    s.conflict,rpmLive?"":"--/",(double)(rpmLive?s.rpm:0),outputMask,mode==MODE_AUTO?"":mode==MODE_TEST?" TEST":" DISARMED");
  if(flexState==FLEX_OK)
    Serial.printf("  FLEX Hz=%.2f ethanol=%.1f%% low_ms=%.3f temp_C=%.1f Input2_ohms=%.0f (provisional)\n",
      (double)flexHz,(double)ethanolPercent(flexHz),(double)flexLowMs,(double)flexTempC(flexLowMs),(double)gmCtsOhms(flexTempC(flexLowMs)));
  else Serial.printf("  FLEX %s\n",flexNames[flexState]);
  if(!isnan(analogTest)) Serial.printf("  AIN TEST %.3fV on Input3-6\n",(double)analogTest);
  else if(!adc.present()) Serial.println("  AIN ADS1115 not found");
  else Serial.printf("  AIN %.3f %.3f %.3f %.3f V\n",(double)(adc.volts(0)*ANALOG_SCALE),(double)(adc.volts(1)*ANALOG_SCALE),
    (double)(adc.volts(2)*ANALOG_SCALE),(double)(adc.volts(3)*ANALOG_SCALE));
  for(unsigned i=0;i<4;++i)
    Serial.printf("  OUT%u row=%u %s\n",i+1,settings.rows[i],rowText(s.rows[settings.rows[i]-1],now));
}
void help(){Serial.println("Commands: STATUS, ARM, DISARM, START, STOP, CAPTURE (toggle raw IO_OUTPUT log), AINTEST <volts>|OFF, WIFI, HELP");}
void printWifi(){Serial.printf("WiFi %s password %s page http://192.168.4.1\n",webSsid,settings.wifiPassword);}
void handleCommand(){
  command[commandUsed]=0;commandUsed=0;
  if(!strcmp(command,"STATUS")) report(millis(),snapshot());
  else if(!strcmp(command,"ARM")){setMode(MODE_AUTO,"AUTO (serial ARM): outputs follow Holley rows.");Serial.println("ARMED: outputs follow Holley rows.");}
  else if(!strcmp(command,"DISARM")){setMode(MODE_DISARMED,"ALL OFF (serial DISARM). Press AUTO to resume.");Serial.println("DISARMED: all outputs OFF until ARM.");}
  else if(!strcmp(command,"WIFI")) printWifi();
  else if(!strcmp(command,"START")){manualStop=false;Serial.println("TX enabled; starts after listen window.");}
  else if(!strcmp(command,"STOP")){manualStop=true;haltTx("Serial STOP");Serial.println("TX stopped until START.");}
  else if(!strcmp(command,"CAPTURE")){captureEnabled=!captureEnabled;Serial.printf("CAPTURE %s\n",captureEnabled?"ON":"OFF");}
  else if(!strcmp(command,"AINTEST OFF")){analogTest=NAN;Serial.println("AIN test off.");}
  else if(!strncmp(command,"AINTEST ",8)){
    char *end;float v=strtof(command+8,&end);
    if(end!=command+8 && !*end && v>=0 && v<=5.5f){analogTest=v;Serial.printf("AIN test %.3fV on Input3-6.\n",(double)v);}
    else Serial.println("AINTEST needs 0..5.5");
  }else if(!strcmp(command,"HELP")) help();
  else if(command[0]) help();
}
void readCommands(){
  // Bounded nonblocking parsing.
  for(unsigned n=0;n<64 && Serial.available();++n){
    char c=Serial.read();
    if(c=='\r' || c=='\n'){
      if(commandOverflow){Serial.println("Command too long.");commandUsed=0;}else handleCommand();
      commandOverflow=false;
    }else if(!commandOverflow){
      if(commandUsed+1<sizeof(command)) command[commandUsed++]=toupper(c);else commandOverflow=true;
    }
  }
}
void printCaptures(){
  RowChange c;
  while(rowChanges && xQueueReceive(rowChanges,&c,0)==pdTRUE)
    if(captureEnabled) Serial.printf("ROW %u bits=%08lX float=%g status=%lu ms=%lu\n",c.row,(unsigned long)c.bits,
      (double)bitsToFloat(c.bits),(unsigned long)c.status,(unsigned long)millis());
}

// ================= Web panel bridge =================
void webControlTick(const BusState &s){
  WebCommand c;
  for(unsigned n=0;n<12 && webCommands && xQueueReceive(webCommands,&c,0)==pdTRUE;++n){
    uint32_t now=millis();
    if(now-c.at>1000) continue; // Do not apply delayed browser commands.
    switch(c.action){
    case WEB_STOP:
      setMode(MODE_DISARMED,"ALL OFF. Press HOLLEY AUTO to resume.");
      xQueueReset(webCommands);return; // STOP overrides anything queued behind it.
    case WEB_AUTO: setMode(MODE_AUTO,"AUTO: outputs follow Holley rows.");break;
    case WEB_TEST:
      if(mode==MODE_TEST) setNotice("Test already active; the timer cannot be extended. ALL OFF to end.");
      else if(!testSafe(now,s)) setNotice("TEST blocked: needs live CAN, RPM 0, and no Holley row ON.");
      else{setMode(MODE_TEST,"OUTPUT TEST active for 30 s. Holley AUTO suspended.");testSince=testHeartbeat=now;}
      break;
    case WEB_HEARTBEAT: if(mode==MODE_TEST) testHeartbeat=now;break;
    case WEB_SET:
      if(mode!=MODE_TEST) setNotice("Output command rejected: arm a test first.");
      else if(c.value) testMask|=1u<<c.index;
      else testMask&=~(1u<<c.index);
      break;
    case WEB_TX_START: manualStop=false;setNotice("Device 1985 TX enabled; starts after the listen window.");break;
    case WEB_TX_STOP: manualStop=true;haltTx("web TX STOP");setNotice("Device 1985 TX stopped until TX START.");break;
    case WEB_AIN_TEST:
      analogTest=c.volts;
      setNotice(isnan(c.volts)?"Analog test volts off.":"Sending fixed test volts on Input3-6 (Bench A).");
      break;
    case WEB_SAVE_ROWS:
      if(mode!=MODE_DISARMED) setNotice("Press ALL OFF / DISARM before saving rows.");
      else if(!saveRows(c.rows)) setNotice("Saving rows failed.");
      else{memcpy(settings.rows,c.rows,sizeof(settings.rows));setNotice("Rows saved.");}
      break;
    case WEB_SET_PASSWORD:
      setNotice(saveWifiPassword(c.text)?"WiFi password saved; applies after the next power cycle.":"Saving WiFi password failed.");
      break;
    }
  }
}

void jsonAdd(char *buf,size_t size,size_t &n,const char *fmt,...){
  if(n>=size) return;
  va_list args;va_start(args,fmt);int w=vsnprintf(buf+n,size-n,fmt,args);va_end(args);
  n+=w>0?w:0;
}
void jsonFloat(char *buf,size_t size,size_t &n,const char *key,float v,bool ok,int digits){
  if(ok && isfinite(v)) jsonAdd(buf,size,n,"\"%s\":%.*f,",key,digits,(double)v);
  else jsonAdd(buf,size,n,"\"%s\":null,",key);
}
void publishWeb(uint32_t now,const BusState &s){
  static uint32_t at=0;
  if(now-at<200) return;
  at=now;
  char json[sizeof(webState)];size_t n=0;const size_t z=sizeof(json);
  bool flexOk=flexState==FLEX_OK, rpmLive=s.rpmSeen && s.rpmValid && now-s.rpmMs<=500;
  jsonAdd(json,z,n,"{\"ready\":true,\"mode\":\"%s\",\"remaining\":%lu,\"notice\":\"%s\",\"mask\":%u,",
    modeNames[mode],(unsigned long)(mode==MODE_TEST?(TEST_MAX_MS-(now-testSince))/1000:0),notice,outputMask);
  jsonAdd(json,z,n,"\"canLive\":%s,\"ecu\":%lu,\"frames\":%lu,\"errors\":%lu,\"missed\":%lu,\"rpmLive\":%s,",
    canLive(now,s)?"true":"false",(unsigned long)s.ecu,(unsigned long)s.total,(unsigned long)canStatus.bus_error_count,
    (unsigned long)canStatus.rx_missed_count,rpmLive?"true":"false");
  jsonFloat(json,z,n,"rpm",s.rpm,rpmLive,0);
  jsonAdd(json,z,n,"\"tx\":\"%s\",\"txFrames\":%lu,\"flex\":\"%s\",",txText(s),(unsigned long)txFrames,flexNames[flexState]);
  jsonFloat(json,z,n,"ethanol",ethanolPercent(flexHz),flexOk,1);
  jsonFloat(json,z,n,"flexHz",flexHz,flexOk,2);
  jsonFloat(json,z,n,"tempF",flexTempC(flexLowMs)*1.8f+32,flexOk,1);
  jsonFloat(json,z,n,"ainTest",analogTest,!isnan(analogTest),3);
  jsonAdd(json,z,n,"\"adc\":%s,\"ain\":[",adc.present()?"true":"false");
  for(unsigned i=0;i<4;++i){
    if(adc.fresh(i,now)) jsonAdd(json,z,n,"%.3f%s",(double)(adc.volts(i)*ANALOG_SCALE),i<3?",":"");
    else jsonAdd(json,z,n,"null%s",i<3?",":"");
  }
  jsonAdd(json,z,n,"],\"rows\":[%u,%u,%u,%u],\"outputs\":[",settings.rows[0],settings.rows[1],settings.rows[2],settings.rows[3]);
  for(unsigned i=0;i<4;++i) jsonAdd(json,z,n,"\"%s\"%s",rowText(s.rows[settings.rows[i]-1],now),i<3?",":"");
  jsonAdd(json,z,n,"],\"ssid\":\"%s\",\"clients\":%u,\"uptime\":%lu}",webSsid,WiFi.softAPgetStationNum(),(unsigned long)now);
  if(n>=z) return; // Truncated: keep the last good state.
  portENTER_CRITICAL(&webMux);memcpy(webState,json,n+1);portEXIT_CRITICAL(&webMux);
}

// LED: solid = TX active + CAN live; slow blink = waiting for CAN; fast blink = fault/conflict.
void ledTick(uint32_t now,const BusState &s){
  bool fault=!canStarted || s.conflict || canStatus.state!=TWAI_STATE_RUNNING;
  bool on=fault?(now%250)<125:txActive?true:(now%1000)<500;
  digitalWrite(PIN_LED,on?LOW:HIGH);
}

// ================= Setup / loop =================
void setup(){
  for(uint8_t p:PIN_OUT){gpio_set_level((gpio_num_t)p,0);gpio_set_direction((gpio_num_t)p,GPIO_MODE_OUTPUT);}
  pinMode(PIN_CAN_SILENT,OUTPUT);setSilent(true);
  pinMode(PIN_LED,OUTPUT);digitalWrite(PIN_LED,HIGH);
  Serial.begin(115200); // USB CDC; no wait for a host.
  WiFi.mode(WIFI_AP); // Radio on first: loadSettings may draw a hardware-random password.
  loadSettings(settings);
  bool webOk=webStart(settings.wifiPassword);
  pinMode(PIN_FLEX,INPUT); // External pull-up required.
  attachInterrupt(digitalPinToInterrupt(PIN_FLEX),flexEdge,CHANGE);
  Wire.begin(PIN_SDA,PIN_SCL,400000);
  adc.begin(Wire);
  rowChanges=xQueueCreate(64,sizeof(RowChange));
  twai_general_config_t g=TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)PIN_CAN_TX,(gpio_num_t)PIN_CAN_RX,TWAI_MODE_NORMAL);
  g.tx_queue_len=8;g.rx_queue_len=256;
  twai_timing_config_t timing=TWAI_TIMING_CONFIG_1MBITS();
  twai_filter_config_t filter=TWAI_FILTER_CONFIG_ACCEPT_ALL();
  esp_err_t r=twai_driver_install(&g,&timing,&filter);
  if(r==ESP_OK){r=twai_start();if(r!=ESP_OK)twai_driver_uninstall();}
  canStarted=r==ESP_OK;
  if(canStarted && xTaskCreate(receiveTask,"CAN_RX",4096,nullptr,3,nullptr)!=pdPASS){
    twai_stop();twai_driver_uninstall();canStarted=false;r=ESP_ERR_NO_MEM;
  }
  quietSince=millis();
  Serial.printf("Flex-over-CAN init CAN=%s ADS1115=%s\n",esp_err_to_name(r),adc.present()?"OK":"MISSING");
  if(webOk) printWifi();else Serial.println("WiFi panel failed to start");
  help();
}

void loop(){
  static uint32_t healthAt=0,reportAt=0;
  uint32_t now=millis();
  if(now-healthAt>=100){healthAt=now;canHealthTick();}
  readCommands();
  updateFlex();
  adc.tick(now);
  BusState s=snapshot();
  webControlTick(s);
  now=millis();
  applyOutputs(now,s);
  txTick(now,s,adc);
  printCaptures();
  publishWeb(now,s);
  ledTick(now,s);
  if(now-reportAt>=1000){reportAt=now;report(now,s);}
  delay(1);
}
