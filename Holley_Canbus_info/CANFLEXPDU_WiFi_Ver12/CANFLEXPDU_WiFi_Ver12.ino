/* Ver12: Wi-Fi PDU + live external flex summary + explicit timed faux source.
   Faux CAN TX defaults OFF. Remove Slim/genuine1985 and RESET before faux test.
   Engine stopped, all Holley outputs OFF, browser heartbeat; maximum60s.
   ESP32-WROOM-32E; ESP32 Dev Module; PSRAM disabled. */
#include <Arduino.h>
#include "Types.h"
#include "WebPanel.h"
#include "TestPolicy.h"
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <driver/gpio.h>
#include <driver/twai.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>

constexpr uint8_t RELAYS[]={32,33,25,26};
// Logical Holley I/O rows, NOT module channel numbers. Row 1 is NOT used.
constexpr uint8_t HOLLEY_ROWS[]={5,6,7,8};
constexpr uint32_t ECU_SERIAL=0x150;
constexpr uint32_t OUTPUT_TIMEOUT_MS=500;
constexpr uint32_t OUTPUT1_BASE=0x1E5E9000;
constexpr uint32_t outputId(unsigned i){return OUTPUT1_BASE+uint32_t(HOLLEY_ROWS[i]-1)*0x4000+ECU_SERIAL;}
constexpr uint16_t CAPACITY=256;
constexpr uint16_t BLACK=0, YELLOW=0xFFE0, GREEN=0x07E0, RED=0xF800;
Adafruit_ST7789 tft(&SPI,14,27,-1);
portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;

IdRecord records[CAPACITY]={};
Counters counters={};
bool canStarted=false,displayEnabled=true,reportsEnabled=false;
esp_err_t initResult=ESP_OK;
twai_status_info_t bus={};
uint32_t prevTotal=0,prevMissed=0,prevOverrun=0,rateAt=0,uiAt=0;
uint32_t dumpAt=0,rowAt=0,reportSeq=0,serialSkipped=0;
float fps=0;
uint32_t missedDelta=0,overrunDelta=0;
int dumpSlot=-1;
bool armed=false,canHealthy=false,autoArmEnabled=true,offDwell=false;
uint32_t offSince=0;
const char *demoFaultReason="NONE";
uint8_t commandedMask=0;
bool manualTest=false; uint8_t manualMask=0;
uint32_t manualSince=0,manualHeartbeat=0,webPublishAt=0;
char webNotice[110]="Ready. AUTO follows Holley; manual testing requires engine stopped and all commands OFF.";
uint32_t healthAt=0,lastRxMissed=0,lastRxOverrun=0;
char commandBuffer[32]={};
unsigned commandUsed=0;
bool commandOverflow=false;
bool demoEnabled=false,demoFault=false,demoStarted=false;
uint32_t demoAt=0,announceAt=0,demoEpoch=0,txQueued=0,txSkipped=0;
float demoEthanol=20, demoTempC=20;
bool fauxActive=false,fauxSweep=false;
uint32_t fauxSince=0,fauxHeartbeat=0,fauxLast=0,fauxAnnounce=0,fauxTxBaseline=0;


void outputsOff(){for(uint8_t p:RELAYS) gpio_set_level((gpio_num_t)p,0);}
void safeStartup(){
  for(uint8_t p:RELAYS){gpio_set_level((gpio_num_t)p,0);gpio_set_direction((gpio_num_t)p,GPIO_MODE_OUTPUT);}
  outputsOff();
}
Counters snapshot(){Counters c;portENTER_CRITICAL(&mux);c=counters;portEXIT_CRITICAL(&mux);return c;}
IdRecord recordSnapshot(int i){IdRecord r;portENTER_CRITICAL(&mux);r=records[i];portEXIT_CRITICAL(&mux);return r;}

void collect(const twai_message_t &m){
  uint32_t now=millis();
  uint8_t len=m.rtr?0:min((int)m.data_length_code,8);
  // Exact ID verified on this Terminator X MAX / firmware 3.1.
  // Holley V3 DBC RPM channel: first four bytes, IEEE754 big endian.
  bool rpmFrame=m.extd && !m.rtr && m.identifier==0x1E005150;
  float rpmValue=0;
  bool rpmValid=false;
  uint8_t rpmStatus=255;
  if(rpmFrame && m.data_length_code==8){
    uint32_t bits=(uint32_t(m.data[0])<<24)|(uint32_t(m.data[1])<<16)|
                  (uint32_t(m.data[2])<<8)|uint32_t(m.data[3]);
    static_assert(sizeof(float)==4,"RPM decoder requires 32-bit float");
    memcpy(&rpmValue,&bits,sizeof(rpmValue));
    rpmStatus=m.data[7];
    // OK, Stall, Synchronized, Syncing; never show a fault-tagged value as ready.
    bool statusOK=rpmStatus==0 || rpmStatus==32 || rpmStatus==33 || rpmStatus==34;
    rpmValid=isfinite(rpmValue) && rpmValue>=0 && rpmValue<=20000 && statusOK;
  }
  portENTER_CRITICAL(&mux);
  if(m.extd && !m.rtr && (m.identifier==0x1E2027C1 || m.identifier==0x1E2067C1)){
    float value=0;bool valid=false;
    if(m.data_length_code==4){
      uint32_t bits=(uint32_t(m.data[0])<<24)|(uint32_t(m.data[1])<<16)|(uint32_t(m.data[2])<<8)|m.data[3];
      memcpy(&value,&bits,4);valid=isfinite(value);
    }
    if(m.identifier==0x1E2027C1){counters.flexSeen=true;counters.flexMs=now;counters.flexHz=value;counters.flexValid=valid&&value>=49.5f&&value<=150.5f;}
    else {counters.tempSeen=true;counters.tempMs=now;counters.tempOhms=value;counters.tempValid=valid&&value>=62&&value<=100700;}
  }
  if(m.extd && !m.rtr && (m.identifier&0x7FF)==0x7C1)counters.moduleConflict=true;
  if(m.extd && !m.rtr && m.identifier==0x0E005150 && m.data_length_code==0)++counters.polls;
  if(m.extd && !m.rtr){
    for(unsigned i=0;i<4;++i)if(m.identifier==outputId(i)){
      counters.outputSeen[i]=true;counters.outputMs[i]=now;
      counters.outputValid[i]=false;counters.outputOn[i]=false;
      counters.outputBits[i]=0;counters.outputStatus[i]=0xFFFFFFFF;
      if(m.data_length_code==8){
        uint32_t bits=(uint32_t(m.data[0])<<24)|(uint32_t(m.data[1])<<16)|(uint32_t(m.data[2])<<8)|m.data[3];
        uint32_t status=(uint32_t(m.data[4])<<24)|(uint32_t(m.data[5])<<16)|(uint32_t(m.data[6])<<8)|m.data[7];
        counters.outputBits[i]=bits;counters.outputStatus[i]=status;
        // Captured OFF=BE float0 + u32 status42; ON=float1 + status43.
        // Reject PWM, arbitrary analog values, and contradictory statuses.
        bool on=bits==0x3F800000 && status==43;
        counters.outputValid[i]=validOutput(bits,status);counters.outputOn[i]=on;
      }
    }
  }
  if(rpmFrame){
    counters.rpmSeen=true;counters.rpmMs=now;counters.rpmStatus=rpmStatus;
    counters.rpmValid=rpmValid;counters.rpm=rpmValue;
  }
  ++counters.total; counters.lastMs=now;counters.lastId=m.identifier;counters.lastExt=m.extd;
  if(m.rtr) ++counters.remoteFrames;
  else if(len) ++counters.dataFrames;
  else ++counters.emptyFrames;
  unsigned start=(m.identifier*2654435761UL+(m.extd?31:0)+(m.rtr?127:0))&(CAPACITY-1);
  IdRecord *r=nullptr;
  for(unsigned n=0;n<CAPACITY;++n){
    IdRecord &candidate=records[(start+n)&(CAPACITY-1)];
    if(!candidate.used){
      candidate.used=true;candidate.id=m.identifier;candidate.ext=m.extd;candidate.rtr=m.rtr;
      ++counters.unique;r=&candidate;break;
    }
    if(candidate.id==m.identifier && candidate.ext==bool(m.extd) && candidate.rtr==bool(m.rtr)){
      r=&candidate;break;
    }
  }
  if(!r) ++counters.untracked;
  else {
    ++r->count;r->lastMs=now;
    if(m.data_length_code<16) r->dlcMask|=uint16_t(1U<<m.data_length_code);
    bool changed=r->count>1 && r->latestLen!=len;
    if(len && !r->firstLen){memcpy(r->first,m.data,len);r->firstLen=len;}
    for(uint8_t b=0;b<len;++b){
      uint8_t bit=1U<<b;
      if(!(r->validMask&bit)){r->lo[b]=r->hi[b]=m.data[b];r->validMask|=bit;}
      else {r->lo[b]=min(r->lo[b],m.data[b]);r->hi[b]=max(r->hi[b],m.data[b]);}
      if(r->count>1 && b<r->latestLen && r->latest[b]!=m.data[b]){
        changed=true;r->changedMask|=bit;
      }
    }
    if(changed) ++r->changes;
    memset(r->latest,0,8);if(len) memcpy(r->latest,m.data,len);r->latestLen=len;
  }
  portEXIT_CRITICAL(&mux);
}
void receiveTask(void*){
  for(;;){
    twai_message_t m;
    // Never wait for a frame; yield one OS tick after a bounded batch.
    for(int i=0;i<128 && twai_receive(&m,0)==ESP_OK;++i) collect(m);
    vTaskDelay(1); // Only this worker yields; screen/serial loop remains independent.
  }
}
bool sendLine(const char *s){
  size_t n=strlen(s);
  if(Serial.availableForWrite()<(int)n){++serialSkipped;return false;}
  Serial.write((const uint8_t*)s,n);return true;
}
bool outputFresh(const Counters &c,unsigned i,uint32_t now){
  return freshOutput(c.outputSeen[i],c.outputValid[i],now,c.outputMs[i],OUTPUT_TIMEOUT_MS);
}
void disarm(){
  if(fauxActive && canStarted)twai_clear_transmit_queue();
  fauxActive=false;armed=false;manualTest=false;manualMask=0;outputsOff();commandedMask=0;
  snprintf(webNotice,sizeof(webNotice),"Outputs OFF. Select HOLLEY AUTO or arm a valid manual test.");
}
void applyOutputs(const Counters &c,uint32_t now){
  uint8_t wanted=0;
  if(armed && canHealthy && canStarted && bus.state==TWAI_STATE_RUNNING){
    for(unsigned i=0;i<4;++i)if(outputFresh(c,i,now) && c.outputOn[i])wanted|=1U<<i;
  }
  if(manualTest && canHealthy && canStarted && bus.state==TWAI_STATE_RUNNING) wanted=manualMask;
  // Perform OFF transitions first; only the main loop owns relay GPIO writes.
  for(unsigned i=0;i<4;++i)if(!(wanted&(1U<<i)))gpio_set_level((gpio_num_t)RELAYS[i],0);
  for(unsigned i=0;i<4;++i)if(wanted&(1U<<i))gpio_set_level((gpio_num_t)RELAYS[i],1);
  commandedMask=wanted;
}
void outputReport(const Counters &c,uint32_t now){
  char s[220];
  snprintf(s,sizeof(s),"OUTPUTS %s GPIOmask=%X map=rows5,6,7,8 RELAY_ONLY\n",armed?"ARMED":"DISARMED",commandedMask);sendLine(s);
  for(unsigned i=0;i<4;++i){
    snprintf(s,sizeof(s),"OUT%u row=%u id=%08lX GPIO=%u cmd=%s signal=%s bits=%08lX status=%lu age=%lums\n",
      i+1,HOLLEY_ROWS[i],(unsigned long)outputId(i),RELAYS[i],commandedMask&(1U<<i)?"ON":"OFF",
      !c.outputSeen[i]?"WAIT":now-c.outputMs[i]>OUTPUT_TIMEOUT_MS?"STALE":!c.outputValid[i]?"INVALID":c.outputOn[i]?"ON":"OFF",
      (unsigned long)c.outputBits[i],(unsigned long)c.outputStatus[i],
      (unsigned long)(c.outputSeen[i]?now-c.outputMs[i]:0));sendLine(s);
  }
}
void handleCommand(){
  commandBuffer[commandUsed]=0;
  if(!strcmp(commandBuffer,"ARM") || !strcmp(commandBuffer,"STOP") || !strcmp(commandBuffer,"DISARM")) disarm();
  if(!strcmp(commandBuffer,"TXSTOP")){webStop("Faux broadcast stopped by Serial TXSTOP; outputs OFF.");demoEnabled=false;demoStarted=false;twai_clear_transmit_queue();sendLine("DEMO TX STOPPED; relays unchanged.\n");}
  else if(!strcmp(commandBuffer,"TXSTART")){sendLine("TXSTART disabled: relay-only firmware. Slim owns flex/temp.\n");}
  else if(!strcmp(commandBuffer,"DISARM") || !strcmp(commandBuffer,"STOP")){
    autoArmEnabled=false;offDwell=false;disarm();if(!strcmp(commandBuffer,"STOP")){demoEnabled=false;demoStarted=false;twai_clear_transmit_queue();}
    sendLine("DISARMED / ALL OUTPUTS OFF; STOP also stops demo TX.\n");
  }else if(!strcmp(commandBuffer,"ARM")){
    Counters c=snapshot();uint32_t now=millis();
    bool allOff=canHealthy && canStarted && bus.state==TWAI_STATE_RUNNING;
    for(unsigned i=0;i<4;++i)allOff=allOff && outputFresh(c,i,now) && !c.outputOn[i];
    if(allOff){autoArmEnabled=true;armed=true;sendLine("ARMED: relays follow Holley logical rows5-8.\n");}
    else sendLine("ARM BLOCKED: require RUNNING CAN and fresh valid OFF on ALL four rows5-8. Use STATUS.\n");
  }else if(!strcmp(commandBuffer,"STATUS")){Counters c=snapshot();outputReport(c,millis());}
  else if(!strcmp(commandBuffer,"IDREPORT") && dumpSlot<0){dumpSlot=0;++reportSeq;}
  else if(!strcmp(commandBuffer,"DISPLAY")){displayEnabled=!displayEnabled;if(displayEnabled)drawStatic();}
  else if(!strcmp(commandBuffer,"HELP"))help();
  else if(commandUsed)sendLine("Unknown command. Use ARM, DISARM, STATUS, IDREPORT, DISPLAY, HELP + Enter.\n");
  commandUsed=0;
}
void readCommands(){
  // Bounded nonblocking parsing. No single character can energize a relay.
  for(unsigned n=0;n<64 && Serial.available();++n){
    char c=Serial.read();
    if(c=='\r' || c=='\n'){
      if(commandOverflow){sendLine("Command overflow; ignored.\n");commandUsed=0;}
      else handleCommand();
      commandOverflow=false;
    }else if(!commandOverflow){
      if(commandUsed+1<sizeof(commandBuffer))commandBuffer[commandUsed++]=c;
      else commandOverflow=true;
    }
  }
}
void bytesHex(char *out,const uint8_t *data,uint8_t len){
  if(!len){strcpy(out,"--");return;}
  for(uint8_t i=0;i<len;++i) snprintf(out+2*i,3,"%02X",data[i]);
}
void rangesHex(char *out,const IdRecord &r,bool maximum){
  for(int i=0;i<8;++i){
    if(r.validMask&(1U<<i)) snprintf(out+2*i,3,"%02X",maximum?r.hi[i]:r.lo[i]);
    else {out[2*i]='-';out[2*i+1]='-';out[2*i+2]=0;}
  }
}
void summary(uint32_t now,const Counters &c){
  char s[480],rpmText[16];
  bool rpmLive=canStarted && bus.state==TWAI_STATE_RUNNING && c.rpmSeen && c.rpmValid && now-c.rpmMs<=500;
  if(rpmLive)snprintf(rpmText,sizeof(rpmText),"%.0f",(double)c.rpm);else strcpy(rpmText,"--");
  const char *rpmState=!canStarted || bus.state!=TWAI_STATE_RUNNING?"CAN_FAULT":!c.rpmSeen?"WAIT":
    now-c.rpmMs>500?"STALE":!c.rpmValid?"INVALID":c.rpmStatus==32?"STALL":c.rpmStatus==34?"SYNCING":"LIVE";
  snprintf(s,sizeof(s),"V12 ms=%lu CAN=%s MODE=NORMAL_ACK fps=%.1f frames=%lu ids=%u payload=%lu empty=%lu rtr=%lu last=%08lX/%s age=%lums missed=%lu(+%lu) overrun=%lu(+%lu) errors=%lu untracked=%lu serialSkip=%lu OUTPUTmode=%s OUTmask=%X RPM=%s RPMstate=%s RPMstatus=%u RPMage=%lums FLEX=EXTERNAL_SLIM\n",
    (unsigned long)now,!canStarted?"ERROR":bus.state!=TWAI_STATE_RUNNING?"FAULT":!c.total?"WAIT":
    now-c.lastMs<=500?"LIVE":now-c.lastMs<=2000?"AGING":"STALE",(double)fps,
    (unsigned long)c.total,c.unique,(unsigned long)c.dataFrames,(unsigned long)c.emptyFrames,
    (unsigned long)c.remoteFrames,(unsigned long)c.lastId,c.lastExt?"EXT":"STD",
    (unsigned long)(c.total?now-c.lastMs:0),(unsigned long)bus.rx_missed_count,
    (unsigned long)missedDelta,(unsigned long)bus.rx_overrun_count,(unsigned long)overrunDelta,
    (unsigned long)bus.bus_error_count,(unsigned long)c.untracked,(unsigned long)serialSkipped,
    armed?"ARMED":"DISARMED",commandedMask,rpmText,rpmState,c.rpmStatus,(unsigned long)(c.rpmSeen?now-c.rpmMs:0));
  sendLine(s);
}
void emitRows(uint32_t now){
  if(dumpSlot<0 || now-rowAt<20) return;
  rowAt=now;
  while(dumpSlot<CAPACITY){
    IdRecord r=recordSnapshot(dumpSlot);
    if(!r.used){++dumpSlot;continue;}
    char first[17],last[17],lo[17],hi[17],s[320];
    bytesHex(first,r.first,r.firstLen);bytesHex(last,r.latest,r.latestLen);
    rangesHex(lo,r,false);rangesHex(hi,r,true);
    snprintf(s,sizeof(s),"ID seq=%lu ms=%lu %s %08lX %s count=%lu dlcMask=%04X age=%lums changes=%lu byteMask=%02X first=%s last=%s min=%s max=%s\n",
      (unsigned long)reportSeq,(unsigned long)now,r.ext?"EXT":"STD",(unsigned long)r.id,
      r.rtr?"RTR":"DATA",(unsigned long)r.count,r.dlcMask,(unsigned long)(millis()-r.lastMs),
      (unsigned long)r.changes,r.changedMask,first,last,lo,hi);
    if(sendLine(s)) ++dumpSlot; // Retry this slot when serial has room.
    return;
  }
  char s[80];snprintf(s,sizeof(s),"END_ID_REPORT seq=%lu\n",(unsigned long)reportSeq);
  if(sendLine(s)){dumpSlot=-1;dumpAt=now;}
}
void textAt(int x,int y,const char *s,int size=1,uint16_t ink=YELLOW){
  tft.setCursor(x,y);tft.setTextSize(size);tft.setTextColor(ink,BLACK);tft.print(s);
}
void row(int y,const char *s,uint16_t ink=YELLOW){tft.fillRect(8,y,304,12,BLACK);textAt(8,y,s,1,ink);}
void drawStatic(){
  tft.fillScreen(BLACK);tft.setTextWrap(false);
  textAt(8,6,"CANFLEX PDU",2);textAt(270,12,"VER12");
  tft.drawFastHLine(8,30,304,YELLOW);
  textAt(8,54,"FLEX/TEMP: EXTERNAL SLIM",1,YELLOW);
  textAt(8,70,"SYSTEM: RUNNING",1,GREEN);
  tft.drawRoundRect(8,96,148,62,4,YELLOW);
  tft.drawRoundRect(164,96,148,62,4,YELLOW);
  textAt(16,103,"RPM / HOLLEY");textAt(172,103,"FLEX SOURCE");
  textAt(172,119,"--",3);textAt(172,145,"SLIM / NO DECODE");
  for(int i=0;i<4;++i){
    int x=8+77*i;char s[16];
    tft.drawRoundRect(x,164,73,32,4,YELLOW);
    snprintf(s,sizeof(s),"OUT%d",i+1);textAt(x+5,169,s);
  }
  textAt(8,228,"DISARMED / OUTPUTS OFF");
}
void drawDynamic(uint32_t now,const Counters &c){
  bool fault=!canStarted || bus.state!=TWAI_STATE_RUNNING;
  row(38,fault?"CAN: DRIVER ERROR":!c.total?"CAN: WAITING":now-c.lastMs<=500?"CAN: LIVE TRAFFIC":
    now-c.lastMs<=2000?"CAN: TRAFFIC AGING":"CAN: NO RECENT TRAFFIC",
    fault?RED:!c.total?YELLOW:now-c.lastMs<=500?GREEN:now-c.lastMs<=2000?YELLOW:RED);
  char s[64];
  snprintf(s,sizeof(s),"FPS %.1f  IDS %u  FRAMES %lu",(double)fps,c.unique,(unsigned long)c.total);row(83,s);
  /* old demo UI disabled
  row(54,demoFault?"DEMO TX FAULT / CHECK SERIAL":demoStarted?"DEMO FLEX/TEMP TX ACTIVE":"DEMO TX WAITING / STOPPED",demoFault?RED:YELLOW);
  snprintf(s,sizeof(s),"TEMP DEMO %.1f C / %.1f F",(double)demoTempC,(double)(demoTempC*1.8f+32));row(70,s,YELLOW);
  tft.fillRect(172,118,132,26,BLACK);
  snprintf(s,sizeof(s),"%.1f",(double)demoEthanol);textAt(172,119,s,3,YELLOW);
  */
  row(54,"WIFI: CANFLEX-PDU  192.168.4.1",YELLOW);
  row(70,"PW: CanFlex-Test11",YELLOW);
  tft.fillRect(172,118,132,26,BLACK);textAt(172,119,fauxActive?"FAUX":"SLIM",2,YELLOW);
  bool live=!fault && c.rpmSeen && c.rpmValid && now-c.rpmMs<=500;
  uint16_t ink=live?(c.rpmStatus==34?YELLOW:GREEN):!fault && !c.rpmSeen?YELLOW:RED;
  tft.fillRect(16,118,132,37,BLACK);
  if(live)snprintf(s,sizeof(s),"%.0f",(double)c.rpm);else strcpy(s,"--");
  textAt(16,118,s,3,ink);
  textAt(16,145,fault?"CAN FAULT":!c.rpmSeen?"WAITING":now-c.rpmMs>500?"STALE":
    !c.rpmValid?"INVALID":c.rpmStatus==32?"ENGINE STALL":c.rpmStatus==34?"SYNCING":"LIVE DECODE",1,ink);
  snprintf(s,sizeof(s),"LAST %s %08lX",c.lastExt?"EXT":"STD",(unsigned long)c.lastId);row(201,c.total?s:"LAST ID --");
  snprintf(s,sizeof(s),"MISSED +%lu OVERRUN +%lu ERR %lu",(unsigned long)missedDelta,
    (unsigned long)overrunDelta,(unsigned long)bus.bus_error_count);
  row(214,s,(missedDelta||overrunDelta)?RED:GREEN);
  for(unsigned i=0;i<4;++i){
    int x=8+77*i;
    bool on=commandedMask&(1U<<i);
    uint16_t color=!armed?YELLOW:outputFresh(c,i,now)?(on?YELLOW:GREEN):RED;
    tft.fillRect(x+4,179,65,12,BLACK);tft.fillCircle(x+9,185,3,color);
    textAt(x+18,181,on?"ON":"OFF",1,color);
  }
  if(fauxActive) {row(228,"SYNTHETIC FLEX TX / ENGINE OFF TEST",YELLOW);return;}
  if(manualTest) {row(228,"MANUAL TEST / 30s MAX / ENGINE STOPPED",YELLOW);return;}
  row(228,armed?"ARMED / HOLLEY ROWS 5-8 / RELAY ONLY":"DISARMED / ALL OUTPUTS OFF",armed?YELLOW:GREEN);
}
void help(){sendLine("V12 commands + Enter: ARM, DISARM (or STOP), STATUS, IDREPORT, DISPLAY, HELP. TXSTART disabled. Boot AUTO ARM after all OFF; STOP inhibits auto ARM. ARM requires all rows5-8 fresh OFF.\n");}
float gmCtsOhms(float c) {
  static const float temperatures[]={-40,-18,-7,4,20,38,70,100,110,120,140};
  static const float resistances[]={100700,25000,13500,7500,3400,1800,450,185,133,102,62};
  constexpr size_t count=sizeof(temperatures)/sizeof(temperatures[0]);
  if(c<=temperatures[0]) return resistances[0];
  if(c>=temperatures[count-1]) return resistances[count-1];
  for(size_t i=1;i<count;++i) if(c<=temperatures[i]) {
    // Interpolate ln(R) versus reciprocal Kelvin for thermistor behavior.
    float k=1.0f/(c+273.15f), k0=1.0f/(temperatures[i-1]+273.15f);
    float k1=1.0f/(temperatures[i]+273.15f);
    float f=(k-k0)/(k1-k0);
    return expf(logf(resistances[i-1])+f*(logf(resistances[i])-logf(resistances[i-1])));
  }
  return resistances[count-1];
}

float demoTriangle(uint32_t elapsed,uint32_t period){float phase=float(elapsed%period)/period;return phase<=0.5f?2*phase:2*(1-phase);}
bool demoFrame(uint32_t id,const uint8_t* data,uint8_t len){
  if(!fauxActive || !canStarted || !canHealthy)return false;
  twai_message_t m={};m.extd=1;m.identifier=id;m.data_length_code=len;memcpy(m.data,data,len);
  if(twai_transmit(&m,0)==ESP_OK){++txQueued;return true;}
  ++txSkipped;webStop("Faux TX enqueue failed. Outputs OFF; check bus before rearming.");return false;
}
void demoFloat(uint32_t id,float value){uint32_t bits;memcpy(&bits,&value,4);uint8_t b[]={uint8_t(bits>>24),uint8_t(bits>>16),uint8_t(bits>>8),uint8_t(bits)};demoFrame(id,b,4);}
void demoTick(uint32_t now,const Counters& c){
  demoEnabled=false;demoStarted=false;
  if(!fauxActive)return;
  if(c.moduleConflict || testExpired(now,fauxSince,fauxHeartbeat,60000) || !webTestSafe(c,now)){
    webStop("Faux test ended: source conflict, timer, heartbeat, or safety condition. Outputs OFF.");return;
  }
  twai_status_info_t status;
  if(twai_get_status_info(&status)!=ESP_OK || status.state!=TWAI_STATE_RUNNING || status.tx_failed_count!=fauxTxBaseline){
    webStop("Faux CAN transmission fault. Check bus before rearming.");return;
  }
  if(status.msgs_to_tx){if(now-fauxLast>250)webStop("Faux CAN TX pending over 250ms. Test stopped.");return;}
  if(now-fauxLast<100)return;
  fauxLast=now;
  if(fauxSweep){demoEthanol=20+20*demoTriangle(now-fauxSince,20000);demoTempC=20+40*demoTriangle(now-fauxSince,30000);}
  if(now-fauxAnnounce>=15500){const uint8_t data[]={0x10};if(!demoFrame(0x0E0027C1,data,1))return;fauxAnnounce=now;}
  demoFloat(0x1E2027C1,demoEthanol+50);
  if(fauxActive)demoFloat(0x1E2067C1,gmCtsOhms(demoTempC));
}
float gmCtsDegrees(float ohms){
  float lo=-40,hi=140;
  for(int i=0;i<20;++i){float mid=(lo+hi)*0.5f;if(gmCtsOhms(mid)>ohms)lo=mid;else hi=mid;}
  return (lo+hi)*0.5f;
}
bool countersConflict(){Counters c=snapshot();return c.moduleConflict;}
void demoReport(){char s[480];snprintf(s,sizeof(s),"DEMO SYNTHETIC TX=%s ETH=%.1f%% TEMP=%.1fC/%.1fF Input1Hz=%.1f Input2_candidateOhms=%.1f queued=%lu skipped=%lu reason=%s conflict=%u txFailed=%lu arbLost=%lu pending=%lu REC=%lu TEC=%lu autoArm=%u\n",demoFault?"FAULT":demoStarted?"ACTIVE":"WAIT/STOP",(double)demoEthanol,(double)demoTempC,(double)(demoTempC*1.8f+32),(double)(demoEthanol+50),(double)gmCtsOhms(demoTempC),(unsigned long)txQueued,(unsigned long)txSkipped,demoFaultReason,countersConflict(),(unsigned long)bus.tx_failed_count,(unsigned long)bus.arb_lost_count,(unsigned long)bus.msgs_to_tx,(unsigned long)bus.rx_error_counter,(unsigned long)bus.tx_error_counter,autoArmEnabled);sendLine(s);}

void autoArmTick(const Counters &c,uint32_t now){
  bool allOff=canHealthy && canStarted && bus.state==TWAI_STATE_RUNNING;
  for(unsigned i=0;i<4;++i)allOff=allOff && outputFresh(c,i,now) && !c.outputOn[i];
  if(!autoArmEnabled || manualTest || fauxActive || armed || now<2000 || !allOff){offDwell=false;return;}
  if(!offDwell){offDwell=true;offSince=now;}
  if(autoArmReady(autoArmEnabled,armed,allOff,now,now-offSince)){armed=true;offDwell=false;sendLine("AUTO ARMED: all four fresh OFF for500ms. Holley now controls relays.\n");}
}
bool webTestSafe(const Counters &c,uint32_t now){
  if(!canStarted || !canHealthy || bus.state!=TWAI_STATE_RUNNING ||
     !c.total || now-c.lastMs>500 || !c.rpmSeen || !c.rpmValid ||
     now-c.rpmMs>500 || c.rpm>0.1f) return false;
  for(unsigned i=0;i<4;++i) if(!outputFresh(c,i,now) || c.outputOn[i])return false;
  return true;
}
void webStop(const char *reason){
  autoArmEnabled=false;offDwell=false;disarm();
  snprintf(webNotice,sizeof(webNotice),"%s",reason);
}
void webControlTick(const Counters &c,uint32_t now){
  if(manualTest && (testExpired(now,manualSince,manualHeartbeat,30000) || !webTestSafe(c,now)))
    webStop("Manual test ended: timer, heartbeat, engine/CAN, or Holley command changed. Outputs OFF.");
  if(!webCommands)return;
  WebCommand request;
  for(unsigned n=0;n<12 && xQueueReceive(webCommands,&request,0)==pdTRUE;++n){
    now=millis();
    if(now-request.at>1000)continue; // Do not apply delayed browser commands.
    if(request.action==WEB_STOP || request.action==WEB_FAUX_STOP){
      webStop("ALL OFF. AUTO inhibited until HOLLEY AUTO or Serial ARM.");
      xQueueReset(webCommands);break; // Stop overrides already queued actions.
    }
    if(request.action==WEB_FAUX_START){
      if(fauxActive || manualTest){snprintf(webNotice,sizeof(webNotice),"End the current test before arming faux mode; active timers cannot be extended.");continue;}
      if(c.moduleConflict){snprintf(webNotice,sizeof(webNotice),"FAUX BLOCKED: external device1985 detected. Disconnect it and RESET PDU first.");continue;}
      if(!fauxMayStart(c.moduleConflict,manualTest,fauxActive,webTestSafe(c,now),now)){snprintf(webNotice,sizeof(webNotice),"FAUX blocked: require startup listen, healthy CAN, stopped RPM and all Holley outputs OFF.");continue;}
      twai_status_info_t status;
      if(twai_get_status_info(&status)!=ESP_OK || status.msgs_to_tx){snprintf(webNotice,sizeof(webNotice),"FAUX blocked: CAN not ready.");continue;}
      disarm();autoArmEnabled=false;offDwell=false;
      fauxSweep=request.sweep;demoEthanol=request.percent;demoTempC=(request.tempF-32)/1.8f;
      fauxSince=fauxHeartbeat=now;fauxLast=now-100;fauxAnnounce=now-15500;
      fauxTxBaseline=status.tx_failed_count;fauxActive=true;
      snprintf(webNotice,sizeof(webNotice),"SYNTHETIC FLEX/TEMP TEST ACTIVE, 60s max. Relay outputs inhibited; engine OFF.");
    }else if(request.action==WEB_AUTO){
      disarm();autoArmEnabled=true;offDwell=false;
      snprintf(webNotice,sizeof(webNotice),"AUTO requested. Waiting for all four valid Holley OFF commands and 500ms dwell.");
    }else if(request.action==WEB_ARM_TEST){
      if(fauxActive){snprintf(webNotice,sizeof(webNotice),"Stop faux broadcasting before manual relay testing.");continue;}
      if(manualTest){snprintf(webNotice,sizeof(webNotice),"Test already active; timer cannot be extended. ALL OFF to end.");continue;}
      if(!webTestSafe(c,now)){snprintf(webNotice,sizeof(webNotice),"TEST blocked: require healthy CAN, fresh RPM zero, and all four Holley outputs OFF.");continue;}
      disarm();autoArmEnabled=false;offDwell=false;
      manualTest=true;manualSince=manualHeartbeat=now;manualMask=0;
      snprintf(webNotice,sizeof(webNotice),"MANUAL TEST ACTIVE. AUTO suspended; unloaded relay commands allowed for 30 seconds.");
    }else if(request.action==WEB_HEARTBEAT){
      if(manualTest)manualHeartbeat=now;
      if(fauxActive)fauxHeartbeat=now;
    }else if(request.action==WEB_SET){
      if(manualTest && webTestSafe(c,now)){
        if(request.value)manualMask|=1U<<request.index;else manualMask&=~(1U<<request.index);
      }else snprintf(webNotice,sizeof(webNotice),"Relay command rejected: arm a valid manual test first.");
    }
  }
}
void publishWeb(const Counters &c,uint32_t now){
  if(now-webPublishAt<200)return;webPublishAt=now;
  const char *states[4];
  for(unsigned i=0;i<4;++i) states[i]=!c.outputSeen[i]?"WAIT":now-c.outputMs[i]>OUTPUT_TIMEOUT_MS?"STALE":!c.outputValid[i]?"INVALID":c.outputOn[i]?"ON":"OFF";
  bool rpmLive=canStarted && canHealthy && bus.state==TWAI_STATE_RUNNING && c.rpmSeen && c.rpmValid && now-c.rpmMs<=500;
  bool live=canStarted && canHealthy && c.total && now-c.lastMs<=500;
  char json[sizeof(webState)];
  snprintf(json,sizeof(json),
    "{\"ready\":true,\"mode\":\"%s\",\"autoPending\":%s,\"mask\":%u,\"rpm\":%.1f,\"rpmLive\":%s,\"canLive\":%s,\"fps\":%.1f,\"remaining\":%lu,\"notice\":\"%s\",\"outputs\":[\"%s\",\"%s\",\"%s\",\"%s\"],\"frames\":%lu,\"ids\":%u,\"errors\":%lu,\"missed\":%lu,\"overrun\":%lu,\"uptime\":%lu,\"clients\":%u}",
    manualTest?"TEST":fauxActive?"FAUX":armed?"AUTO":"OFF",autoArmEnabled&&!armed?"true":"false",commandedMask,
    (double)(rpmLive?c.rpm:0),rpmLive?"true":"false",live?"true":"false",(double)fps,
    (unsigned long)(manualTest?(30000-(now-manualSince))/1000:0),webNotice,
    states[0],states[1],states[2],states[3],(unsigned long)c.total,c.unique,
    (unsigned long)bus.bus_error_count,(unsigned long)bus.rx_missed_count,(unsigned long)bus.rx_overrun_count,
    (unsigned long)now,WiFi.softAPgetStationNum());
  bool flexLive=live&&c.flexSeen&&c.flexValid&&now-c.flexMs<=1000;
  bool tempLive=live&&c.tempSeen&&c.tempValid&&now-c.tempMs<=1000;
  size_t pos=strlen(json);if(pos && json[pos-1]=='}')--pos;
  snprintf(json+pos,sizeof(json)-pos,
    ",\"flexSeen\":%s,\"flexLive\":%s,\"flexHz\":%.2f,\"ethanol\":%.2f,\"tempLive\":%s,\"liveTempF\":%.2f,\"fauxActive\":%s,\"fauxBlocked\":%s,\"fauxPercent\":%.2f,\"fauxTempF\":%.2f,\"fauxRemaining\":%lu,\"fauxQueued\":%lu}",
    c.flexSeen?"true":"false",flexLive?"true":"false",(double)(flexLive?c.flexHz:0),(double)(flexLive?constrain(c.flexHz-50,0.0f,100.0f):0),
    tempLive?"true":"false",(double)(tempLive?gmCtsDegrees(c.tempOhms)*1.8f+32:0),
    fauxActive?"true":"false",c.moduleConflict?"true":"false",(double)demoEthanol,(double)(demoTempC*1.8f+32),
    (unsigned long)(fauxActive?(60000-(now-fauxSince))/1000:0),(unsigned long)txQueued);
  portENTER_CRITICAL(&webMux);memcpy(webState,json,sizeof(webState));portEXIT_CRITICAL(&webMux);
}
void setup(){
  safeStartup();
  // HardwareSerial supports a TX buffer; native USB CDC does not expose it.
#if !ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxBufferSize(2048);
#endif
  Serial.begin(115200);
  SPI.begin(18,-1,23,14);tft.init(240,320);tft.setRotation(1);tft.setSPISpeed(40000000);drawStatic();
  twai_general_config_t g=TWAI_GENERAL_CONFIG_DEFAULT(GPIO_NUM_17,GPIO_NUM_16,TWAI_MODE_NORMAL);
  g.tx_queue_len=8;g.rx_queue_len=1024; // Used ONLY by explicitly armed faux bench source.
  twai_timing_config_t timing=TWAI_TIMING_CONFIG_1MBITS();
  twai_filter_config_t filter=TWAI_FILTER_CONFIG_ACCEPT_ALL();
  initResult=twai_driver_install(&g,&timing,&filter);
  if(initResult==ESP_OK){
    initResult=twai_start();canStarted=initResult==ESP_OK;
    if(!canStarted) twai_driver_uninstall();
  }
  if(canStarted && xTaskCreatePinnedToCore(receiveTask,"CAN_RX",4096,nullptr,3,nullptr,0)!=pdPASS){
    twai_stop();twai_driver_uninstall();canStarted=false;initResult=ESP_ERR_NO_MEM;
  }
  char s[180];snprintf(s,sizeof(s),"CANFLEXPDU_WiFi_Ver12 init=%s queue=1024 slots=256 relays=OFF mode=NORMAL_ACK bitrate=1Mbps appTx=OFF_UNTIL_WEB_FAUX_ARM\n",esp_err_to_name(initResult));sendLine(s);help();
  webStart();
  Serial.printf("WiFi %s password %s page http://192.168.4.1\n",WEB_SSID,WEB_PASSWORD);
  rateAt=millis();dumpAt=rateAt;
}
void loop(){
  uint32_t now=millis();
  if(canStarted && now-healthAt>=100){
    healthAt=now;
    esp_err_t result=twai_get_status_info(&bus);
    canHealthy=result==ESP_OK && bus.state==TWAI_STATE_RUNNING && bus.tx_error_counter<128 && bus.rx_error_counter<128 && bus.rx_missed_count==lastRxMissed && bus.rx_overrun_count==lastRxOverrun;
    if(!canHealthy){
      if(armed)sendLine("DISARM: CAN fault or receive loss. Reset/recheck, then ARM when all OFF.\n");
      autoArmEnabled=false;offDwell=false;disarm();
    }
    lastRxMissed=bus.rx_missed_count;lastRxOverrun=bus.rx_overrun_count;
  }
  readCommands();
  Counters relaySnapshot=snapshot();now=millis();webControlTick(relaySnapshot,now);autoArmTick(relaySnapshot,now);applyOutputs(relaySnapshot,now);demoTick(now,relaySnapshot);
  if(now-rateAt>=1000){
    Counters c=snapshot();now=millis(); // Read clock after shared RX timestamps.
    fps=(c.total-prevTotal)*1000.0f/(now-rateAt);prevTotal=c.total;rateAt=now;
    missedDelta=bus.rx_missed_count-prevMissed;prevMissed=bus.rx_missed_count;
    overrunDelta=bus.rx_overrun_count-prevOverrun;prevOverrun=bus.rx_overrun_count;
    summary(now,c);
    outputReport(c,now);
  }
  if(displayEnabled && now-uiAt>=250){
    Counters c=snapshot();now=millis();uiAt=now;drawDynamic(now,c);
  }
  if(reportsEnabled && dumpSlot<0 && now-dumpAt>=10000){dumpSlot=0;++reportSeq;}
  publishWeb(snapshot(),millis());
  emitRows(now);
  delay(1);
}











