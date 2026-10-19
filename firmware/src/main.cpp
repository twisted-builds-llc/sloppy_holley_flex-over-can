/* Sloppy Holley Flex-over-CAN -- ESP32-C3, Arduino-ESP32 3.x (pioarduino, see platformio.ini).
   To Holley as CAN device 1985: Input1 flex Hz, Input2 fuel temp as GM CTS ohms, Input3-6 analog volts.
   From Holley: IO_OUTPUT rows (config.h) -> OUT1-4 low-side, on/off. PWM waits for Bench B capture.
   Protocol status: docs/CAN_PROTOCOL.md. Not for engine control before docs/PROGRESS.md validations.
   Serial 115200, commands + Enter: STATUS, ARM, DISARM, START, STOP, CAPTURE, AINTEST <volts>|OFF, HELP. */
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

void txTick(uint32_t now,const BusState &s,const Ads1115 &adc){
  bool canLive=canStarted && canStatus.state==TWAI_STATE_RUNNING && s.total && now-s.lastMs<=500;
  if(!txActive){
    // Start only after a silent listen window with live traffic and no competing device 1985.
    if(manualStop || s.conflict || !canLive || now-quietSince<quietNeeded) return;
    txFailedBaseline=canStatus.tx_failed_count;polledSeen=s.polls;
    setSilent(false);txActive=true;lastSend=now-TX_PERIOD_MS;
    Serial.println("[TX ACTIVE] device 1985 broadcasting");
    announce(now);
    return;
  }
  if(s.conflict){haltTx("another device 1985 (0x7C1) detected; reset after removing it");return;}
  if(!canLive || canStatus.tx_failed_count!=txFailedBaseline ||
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
// Outputs follow fresh, valid Holley commands. Stale/invalid/missing -> OFF.
// DISARM latches all OFF until ARM.
bool disarmed=false;
uint8_t outputMask=0;

void applyOutputs(uint32_t now,const BusState &s){
  uint8_t wanted=0;
  if(!disarmed && now>=BOOT_GUARD_MS && s.ecu){
    for(unsigned i=0;i<4;++i){
      const RowState &r=s.rows[OUTPUT_ROW[i]-1];
      if(freshOutput(r.seen,r.valid,now,r.ms,OUTPUT_TIMEOUT_MS) && r.on) wanted|=1u<<i;
    }
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
    s.conflict,rpmLive?"":"--/",(double)(rpmLive?s.rpm:0),outputMask,disarmed?" DISARMED":"");
  if(flexState==FLEX_OK)
    Serial.printf("  FLEX Hz=%.2f ethanol=%.1f%% low_ms=%.3f temp_C=%.1f Input2_ohms=%.0f (provisional)\n",
      (double)flexHz,(double)ethanolPercent(flexHz),(double)flexLowMs,(double)flexTempC(flexLowMs),(double)gmCtsOhms(flexTempC(flexLowMs)));
  else Serial.printf("  FLEX %s\n",flexNames[flexState]);
  if(!isnan(analogTest)) Serial.printf("  AIN TEST %.3fV on Input3-6\n",(double)analogTest);
  else if(!adc.present()) Serial.println("  AIN ADS1115 not found");
  else Serial.printf("  AIN %.3f %.3f %.3f %.3f V\n",(double)(adc.volts(0)*ANALOG_SCALE),(double)(adc.volts(1)*ANALOG_SCALE),
    (double)(adc.volts(2)*ANALOG_SCALE),(double)(adc.volts(3)*ANALOG_SCALE));
  for(unsigned i=0;i<4;++i)
    Serial.printf("  OUT%u row=%u %s\n",i+1,OUTPUT_ROW[i],rowText(s.rows[OUTPUT_ROW[i]-1],now));
}
void help(){Serial.println("Commands: STATUS, ARM, DISARM, START, STOP, CAPTURE (toggle raw IO_OUTPUT log), AINTEST <volts>|OFF, HELP");}
void handleCommand(){
  command[commandUsed]=0;commandUsed=0;
  if(!strcmp(command,"STATUS")) report(millis(),snapshot());
  else if(!strcmp(command,"ARM")){disarmed=false;Serial.println("ARMED: outputs follow Holley rows.");}
  else if(!strcmp(command,"DISARM")){disarmed=true;Serial.println("DISARMED: all outputs OFF until ARM.");}
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
  help();
}

void loop(){
  static uint32_t healthAt=0,reportAt=0;
  uint32_t now=millis();
  if(now-healthAt>=100){healthAt=now;canHealthTick();}
  readCommands();
  updateFlex();
  adc.tick(now);
  BusState s=snapshot();now=millis();
  applyOutputs(now,s);
  txTick(now,s,adc);
  printCaptures();
  ledTick(now,s);
  if(now-reportAt>=1000){reportAt=now;report(now,s);}
  delay(1);
}
