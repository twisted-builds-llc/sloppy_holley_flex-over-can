/* CAN_Flex_Slim_Ver5 -- ESP32-C3 SuperMini, Arduino-ESP32 3.x
   Board: ESP32C3 Dev Module; USB CDC On Boot: Enabled; Serial: 115200.
   No external libraries required. Optional RGB uses core rgbLedWrite().

   WIRING / READ BEFORE CONNECTING:
   GPIO3 <- Continental open-collector signal, external 4.7k pull-up to 3.3V.
   GPIO4 -> transceiver TX; GPIO5 <- transceiver RX; common ground.
   CANH/CANL -> verified Holley CANH/CANL, short twisted-pair stub.
   For confirmed Adafruit CAN Pal #5708: Vcc=3.3V, GND=GND,
   SLNT HIGH for initial passive testing. For Ver5 transmission, power OFF,
   change SLNT to LOW per breakout documentation, then power on again.
   Disable its termination on an already properly terminated bus.
   EXACT BREAKOUT CHIP, POWER AND LOGIC LEVELS MUST BE VERIFIED FIRST:
   earlier blanket assumptions of 3.3V power may be incorrect. CAN Pal has
   a 5V charge pump; bare TJA1051/other boards may require separate 5V/VIO.
   ESP32 GPIOs are NOT 5V tolerant. Never apply vehicle 12V to GPIO or 3V3.
   Verify sensor pinout, supply and open-collector behavior by part number.
   Use protected automotive power; avoid USB/vehicle ground/backfeed hazards.
   If signal is shared with an ECU's 5V pull-up, isolate/level-shift first.
   GPIO8 is a strap pin; ordinary SuperMini has a blue LED, not WS2812.
   Enable RGB only for verified WS2812 hardware with suitable power/data levels
   and no boot-strap interference; onboard LED loading may require modification.

   Starts listen-only, automatically arms after valid sensor + active CAN.
   Device 1985 (0x7C1) must be disconnected before START. Preserve termination.
   User confirmed earlier 70..90Hz float test produced 20..40% on SAME tune.
   Input1 frequency Hz: EXT29 0x1E2027C1, DLC4, big-endian IEEE754 float.
   Announcement 0x0E0027C1 DLC1 0x10 every 15.5s and on 0x0E005150 DLC0 poll.
   Input2 0x1E2067C1 DLC4 BE float: candidate GM CTS equivalent ohms.
   GM curve approximate; exact Holley preset and Input2 units need bench validation.
   No guessed sensor-fault flag. Missing/invalid pulse or CAN fault stops CAN,
   clears queued frames, retries after 3s. STOP inhibits restart until START.
   Competing source 0x7C1 latches restart off until reset. Holley timeout behavior
   MUST be checked key-on/engine-off before engine use; it may hold last value.
   Software disarm is not hardware silence: SLNT HIGH required for that due
   to C3 errata. Normal mode also ACKs received traffic.
   1Mbps is the requested test setting, not auto-detected or guaranteed for ECU.
   Extended IDs are recorded, not decoded. Low-byte source is only a candidate
   convention; conflict detection uses the previous tests' low 11-bit 0x7C1.
   PROVISIONAL Continental-style conversion, verify actual sensor specification:
   ethanol %=Hz-50 (50..150Hz); temp C=41.25*LOW_ms-81.25 (1..5ms).
   Never use these provisional readings for engine control.
*/
#include <Arduino.h>
#include <math.h>
#include "driver/twai.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_err.h"
#if !defined(CONFIG_IDF_TARGET_ESP32C3)
#error Select ESP32C3 Dev Module
#endif
#ifndef ENABLE_WS2812
#define ENABLE_WS2812 0  // set 1 only after checking actual hardware
#endif
constexpr uint8_t FLEX_PIN=3, TX_PIN=4, RX_PIN=5, RGB_PIN=8;
constexpr uint32_t FLEX_TIMEOUT_US=100000, CAN_TIMEOUT_MS=2000;
portMUX_TYPE pulseMux=portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t lastFall=0, edgeTime=0, lowWidth=0, periodUs=0,
                  sampleLow=0, sampleTime=0, sampleCount=0;
volatile bool haveFall=false, haveLow=false, sawEdge=false;

void ARDUINO_ISR_ATTR flexEdge() {
  uint32_t now=(uint32_t)esp_timer_get_time();
  bool high=gpio_get_level((gpio_num_t)FLEX_PIN);
  portENTER_CRITICAL_ISR(&pulseMux);
  edgeTime=now; sawEdge=true;
  if (!high) {
    // Pair previous completed LOW with its falling-to-falling period.
    if (haveFall && haveLow) {
      periodUs=now-lastFall; sampleLow=lowWidth;
      sampleTime=now; ++sampleCount;
    }
    lastFall=now; haveFall=true; haveLow=false;
  } else if (haveFall) {
    lowWidth=now-lastFall; haveLow=true;
  }
  portEXIT_CRITICAL_ISR(&pulseMux);
}

enum FlexState { NO_PULSE, INCOMPLETE, BAD_FREQUENCY, BAD_WIDTH, FLEX_OK };
const char* flexNames[]={"NO_PULSE", "INCOMPLETE_OR_STALE", "FREQUENCY_RANGE", "LOW_WIDTH_RANGE", "OK"};
FlexState flexState=NO_PULSE;
float hz=0, ethanol=0, lowMs=0, tempC=0;
bool canReady=false, haveCan=false, recentCanError=false;
uint32_t lastCan=0, lastError=0, lastReport=0, lastHealth=0;
uint64_t rxTotal=0, rxExt=0, rxStd=0, rxRtr=0, untracked=0;
// Bounded tables: no heap allocation or per-frame Serial logging.
struct IdEntry { uint32_t id; uint64_t count; };
IdEntry ids[64]; uint8_t idUsed=0;
uint64_t sourceCounts[256]={};
uint32_t previousBusErrors=0, previousMissed=0, previousOverrun=0;
int previousDriverState=-1, previousFlex=-1, previousCan=-1;
esp_err_t previousRxResult=ESP_OK;
bool armed=false, halted=false, moduleSeen=false;
bool manualStop=false, driverInstalled=false;
uint32_t retryAt=0, listenSince=0;
uint32_t lastSend=0, lastAnnounce=0, validSince=0;
uint64_t txInput=0, txAnnounce=0, txTemp=0;
char command[16]; uint8_t commandUsed=0; bool commandOverflow=false;

void haltTx(const char* reason) {
  armed=false; halted=true;
  if(canReady) { twai_stop(); twai_clear_transmit_queue(); }
  canReady=false;
  retryAt=millis();
  Serial.printf("[STOPPED] %s. Auto retry after 3s unless STOP/conflict. No fault flag sent.\n",reason);
}

bool sendFrame(uint32_t id, uint8_t dlc, const uint8_t* bytes) {
  if(!armed || !canReady) return false;
  twai_message_t m={}; m.identifier=id; m.extd=1; m.data_length_code=dlc;
  memcpy(m.data,bytes,dlc);
  m.ss=0; // Normal arbitration/retransmission, matching successful Ver9.
  esp_err_t r=twai_transmit(&m,0);
  if(r!=ESP_OK) { haltTx(esp_err_to_name(r)); return false; }
  return true;
}

void announce() {
  const uint8_t d[]={0x10};
  if(sendFrame(0x0E0027C1,1,d)) ++txAnnounce;
}

bool installCan(twai_mode_t mode) {
  twai_general_config_t g=TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)TX_PIN,(gpio_num_t)RX_PIN,mode);
  g.tx_queue_len=mode==TWAI_MODE_NORMAL?8:0; g.rx_queue_len=128;
  twai_timing_config_t timing=TWAI_TIMING_CONFIG_1MBITS();
  twai_filter_config_t filter=TWAI_FILTER_CONFIG_ACCEPT_ALL();
  esp_err_t r=twai_driver_install(&g,&timing,&filter);
  driverInstalled=r==ESP_OK;
  if(r==ESP_OK) { r=twai_start(); if(r!=ESP_OK) {twai_driver_uninstall(); driverInstalled=false;} }
  Serial.printf("[CAN init] %s mode=%s\n",esp_err_to_name(r),mode==TWAI_MODE_NORMAL?"NORMAL":"LISTEN_ONLY");
  canReady=r==ESP_OK;
  if(canReady && mode==TWAI_MODE_LISTEN_ONLY) {
    listenSince=millis(); haveCan=false;
  }
  return canReady;
}

void handleCommand() {
  command[commandUsed]=0;
  if(!strcmp(command,"STOP")) {manualStop=true; haltTx("Serial STOP; automatic restart disabled");}
  else if(!strcmp(command,"START")) {
    manualStop=false;
    if(halted || !canReady) Serial.println("Automatic restart enabled; waiting for recovery.");
    else if(armed) Serial.println("Already armed.");
    else if(moduleSeen) Serial.println("START blocked: another 0x7C1 device seen. Remove genuine module then reset.");
    else if(flexState!=FLEX_OK || !validSince || millis()-validSince<1000)
      Serial.println("START blocked: need 1 second of valid sensor readings.");
    else {
      twai_stop(); twai_driver_uninstall(); driverInstalled=false; canReady=false;
      if(installCan(TWAI_MODE_NORMAL)) {
        previousBusErrors=previousMissed=previousOverrun=0;
        previousDriverState=-1; armed=true;
        lastSend=lastAnnounce=millis(); announce();
        Serial.println("ARMED: measured frequency to Input1, GM-style CTS ohms to Input2. Verify Holley key-on/engine-off first.");
      } else {halted=true; retryAt=millis();}
    }
  } else if(commandUsed) Serial.println("Commands: START or STOP, newline.");
  commandUsed=0;
}

void automaticStart(uint32_t now) {
  if(manualStop || moduleSeen || armed) return;
  if(halted || !canReady) {
    if(now-retryAt<3000) return;
    retryAt=now;
    if(driverInstalled) {
      twai_stop(); twai_driver_uninstall(); driverInstalled=false;
    }
    halted=false;
    if(!installCan(TWAI_MODE_LISTEN_ONLY)) {halted=true; return;}
    previousDriverState=-1;
    previousBusErrors=previousMissed=previousOverrun=0;
    previousRxResult=ESP_OK;
    return;
  }
  // Fresh listen window gives conflict traffic a chance to be observed.
  // Wait for an active bus rather than transmitting into a disconnected cable.
  if(now-listenSince<1500 || !haveCan || now-lastCan>500 ||
     flexState!=FLEX_OK || !validSince || now-validSince<1000) return;
  strcpy(command,"START"); commandUsed=5; handleCommand();
}

void readCommands() {
  while(Serial.available()) {
    char c=(char)Serial.read();
    if(c=='\r' || c=='\n') {
      if(!commandOverflow) handleCommand(); else { commandUsed=0; Serial.println("Command too long."); }
      commandOverflow=false;
    } else if(!commandOverflow) {
      if(commandUsed<sizeof(command)-1) command[commandUsed++]=c;
      else commandOverflow=true;
    }
  }
}

// Approximate GM-style curve, Electromotive TECgt manual Table G.3.3, p91.
// https://electromotive.com/wp-content/uploads/2015/05/Tec-gt-Manual-2.3.pdf
// Not an extracted Holley GM CTS table. Verify display against Serial first.
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

void runTx(uint32_t now) {
  if(!armed) return;
  if(flexState!=FLEX_OK) { haltTx("sensor missing, stale, or invalid"); return; }
  twai_status_info_t s;
  if(twai_get_status_info(&s)!=ESP_OK) { haltTx("CAN status unavailable"); return; }
  if(s.state!=TWAI_STATE_RUNNING || s.tx_failed_count || s.tx_error_counter>=128 || s.rx_error_counter>=128) {
    Serial.printf("[CAN fault detail] state=%d txFailed=%lu busErr=%lu arbLost=%lu TEC=%lu REC=%lu txPending=%lu\n", (int)s.state, (unsigned long)s.tx_failed_count, (unsigned long)s.bus_error_count, (unsigned long)s.arb_lost_count, (unsigned long)s.tx_error_counter, (unsigned long)s.rx_error_counter, (unsigned long)s.msgs_to_tx);
    haltTx("CAN TX failure or controller fault"); return;
  }
  if(s.msgs_to_tx && now-lastSend>250) { haltTx("CAN TX pending beyond 250ms; no stale retry allowed"); return; }
  if(now-lastAnnounce>=15500) { lastAnnounce=now; announce(); }
  if(now-lastSend>=100) {
    if(s.msgs_to_tx) return; // Do not queue sensor values behind a blocked frame.
    lastSend=now;
    // Clamp only the small endpoint tolerance to nominal 50..150Hz.
    float value=constrain(hz,50.0f,150.0f); uint32_t bits;
    static_assert(sizeof(bits)==sizeof(value),"32-bit float required");
    memcpy(&bits,&value,sizeof(bits));
    uint8_t d[]={uint8_t(bits>>24),uint8_t(bits>>16),uint8_t(bits>>8),uint8_t(bits)};
    if(sendFrame(0x1E2027C1,4,d)) ++txInput;
    value=gmCtsOhms(tempC); memcpy(&bits,&value,sizeof(bits));
    d[0]=uint8_t(bits>>24); d[1]=uint8_t(bits>>16); d[2]=uint8_t(bits>>8); d[3]=uint8_t(bits);
    if(sendFrame(0x1E2067C1,4,d)) ++txTemp;
  }
}

void updateFlex() {
  uint32_t p,l,t,e,n; bool edges;
  portENTER_CRITICAL(&pulseMux);
  p=periodUs; l=sampleLow; t=sampleTime; e=edgeTime;
  n=sampleCount; edges=sawEdge;
  uint32_t now=(uint32_t)esp_timer_get_time();
  // Drop partial measurement after silence; don't bridge disconnected bursts.
  if (!edges || now-e>FLEX_TIMEOUT_US) { haveFall=false; haveLow=false; }
  portEXIT_CRITICAL(&pulseMux);
  if (!edges || now-e>FLEX_TIMEOUT_US) flexState=NO_PULSE;
  else if (!n || now-t>FLEX_TIMEOUT_US || !p) flexState=INCOMPLETE;
  else {
    hz=1000000.0f/p; lowMs=l/1000.0f;
    ethanol=constrain(hz-50.0f,0.0f,100.0f); tempC=41.25f*lowMs-81.25f;
    // Allow endpoint jitter: fixes Ver1 chatter around exactly 50Hz.
    if (hz<49.5f || hz>150.5f) flexState=BAD_FREQUENCY;
    else if (l>=p || l<1000 || l>5000) flexState=BAD_WIDTH;
    else flexState=FLEX_OK;
  }
  if(flexState==FLEX_OK) { if(!validSince) validSince=millis(); }
  else validSince=0;
  if (previousFlex!=(int)flexState) {
    Serial.printf("[FLEX state] %s\n",flexNames[flexState]);
    previousFlex=flexState;
  }
}

void pollCan() {
  if (!canReady) return;
  twai_message_t msg;
  // Bound drain work so flex/LED reporting remains responsive under heavy CAN.
  for (int i=0;i<128;++i) {
    esp_err_t r=twai_receive(&msg,0);
    if (r==ESP_ERR_TIMEOUT) break;
    if (r!=ESP_OK) {
      if (r!=previousRxResult) Serial.printf("[CAN RX error] %s\n",esp_err_to_name(r));
      previousRxResult=r; break;
    }
    if (previousRxResult!=ESP_OK) Serial.println("[CAN RX error] cleared");
    previousRxResult=ESP_OK;
    ++rxTotal; haveCan=true; lastCan=millis();
    if(msg.extd && !msg.rtr) {
      if((msg.identifier & 0x7FFU)==0x7C1U) {
        moduleSeen=true;
        if(armed) { haltTx("another device using 0x7C1 detected"); return; }
      }
      if(msg.identifier==0x0E005150 && msg.data_length_code==0 && armed) announce();
    }
    if (msg.rtr) ++rxRtr;
    if (!msg.extd) { ++rxStd; continue; }
    ++rxExt; ++sourceCounts[msg.identifier & 0xFF];
    uint8_t j=0;
    for (;j<idUsed;++j) if (ids[j].id==msg.identifier) break;
    if (j<idUsed) ++ids[j].count;
    else if (idUsed<64) { ids[idUsed++]={msg.identifier,1}; }
    else ++untracked;
  }
}

void checkHealth(uint32_t now) {
  if (now-lastHealth<250) return;
  lastHealth=now;
  if (canReady) {
    twai_status_info_t s;
    esp_err_t r=twai_get_status_info(&s);
    if (r==ESP_OK) {
      if (previousDriverState!=(int)s.state) {
        Serial.printf("[CAN driver state] %d (0 stopped,1 running,2 bus-off,3 recovering)\n",s.state);
        previousDriverState=s.state;
      }
      // Log entry/clear only, cumulative counters remain in periodic telemetry.
      if (s.bus_error_count!=previousBusErrors || s.rx_missed_count!=previousMissed || s.rx_overrun_count!=previousOverrun) {
        if (!recentCanError) Serial.println("[CAN error] new bus errors or receive loss");
        recentCanError=true; lastError=now;
      }
      previousBusErrors=s.bus_error_count; previousMissed=s.rx_missed_count;
      previousOverrun=s.rx_overrun_count;
    }
  }
  if (recentCanError && now-lastError>5000) {
    recentCanError=false; Serial.println("[CAN error] quiet for 5 seconds");
  }
  int state=!canReady?0:(!haveCan || now-lastCan>CAN_TIMEOUT_MS)?1:2;
  if (state!=previousCan) {
    Serial.printf("[CAN activity] %s\n",state==0?"INIT_FAILED":state==1?"NO_RECENT_FRAMES (does not prove disconnected)":"RX_ACTIVE");
    previousCan=state;
  }
}

void updateLed(uint32_t now) {
#if ENABLE_WS2812
  uint8_t r=0,g=0,b=0;
  bool on=(now%1000)<250;
  if (halted || !canReady || recentCanError || previousDriverState==TWAI_STATE_BUS_OFF) { if(on) r=24; }
  else if (flexState!=FLEX_OK) { if(on) {r=20;g=8;} }
  else if (!haveCan || now-lastCan>CAN_TIMEOUT_MS) { if(on) b=24; }
  else if(armed) g=16;
  else { g=8; b=16; } // cyan: valid sensor / CAN visible, still disarmed
  static uint32_t oldColor=0xFFFFFFFF;
  uint32_t color=((uint32_t)r<<16)|((uint32_t)g<<8)|b;
  if(color!=oldColor) { rgbLedWrite(RGB_PIN,r,g,b); oldColor=color; }
#endif
}

void report(uint32_t now) {
  if (now-lastReport<1000) return;
  lastReport=now;
  Serial.printf("Ver5 tx=%s flex=%s ",armed?"ARMED":halted?"STOPPED":"DISARMED",flexNames[flexState]);
  if (flexState==NO_PULSE || flexState==INCOMPLETE) Serial.print("Hz=NA ethanol=NA low_ms=NA temp_C=NA ");
  else {
    Serial.printf("Hz=%.2f low_ms=%.3f ",hz,lowMs);
    if (flexState==FLEX_OK) Serial.printf("ethanol_provisional=%.2f%% temp_C_provisional=%.2f ",ethanol,tempC);
    else Serial.print("ethanol=INVALID temp_C=INVALID ");
  }
  Serial.printf("RX=%llu EXT29=%llu STD11=%llu RTR=%llu bus_errors=%lu missed=%lu overrun=%lu untracked=%llu\n",
    (unsigned long long)rxTotal,(unsigned long long)rxExt,(unsigned long long)rxStd,(unsigned long long)rxRtr,
    (unsigned long)previousBusErrors,(unsigned long)previousMissed,(unsigned long)previousOverrun,(unsigned long long)untracked);
  Serial.printf("TX input_queued=%llu announce_queued=%llu (queued is not ECU acceptance)\n",
    (unsigned long long)txInput,(unsigned long long)txAnnounce);
  if(flexState==FLEX_OK) Serial.printf("TEMP Input2_candidate_ohms=%.1f temp_F_provisional=%.2f temp_queued=%llu GM_curve=APPROXIMATE\n",gmCtsOhms(tempC),tempC*1.8f+32.0f,(unsigned long long)txTemp);
  static uint32_t lastIds=0;
  if(now-lastIds>=10000) {
    lastIds=now;
    Serial.println("[Observed EXT29 IDs: cumulative; max 64, no Holley decoding]");
    for(uint8_t i=0;i<idUsed;++i) Serial.printf(" ID=0x%08lX count=%llu\n",(unsigned long)ids[i].id,(unsigned long long)ids[i].count);
    Serial.println("[Candidate source = ID low byte; convention UNVERIFIED]");
    for(int i=0;i<256;++i) if(sourceCounts[i]) Serial.printf(" source?=0x%02X count=%llu\n",i,(unsigned long long)sourceCounts[i]);
  }
}

void setup() {
  Serial.begin(115200); // No wait for USB host: acquisition starts immediately.
  pinMode(FLEX_PIN,INPUT); // external 3.3V pull-up REQUIRED
  attachInterrupt(digitalPinToInterrupt(FLEX_PIN),flexEdge,CHANGE);
  Serial.println("CAN_Flex_Slim_Ver5: measured Hz -> device 1985 Input1, same user-confirmed tune.");
  Serial.println("AUTO START: valid sensor + active CAN + 1.5s listen window. Remove genuine 1985.");
  Serial.println("AUTO RETRY after faults, 3s backoff. STOP disables auto; START enables it. Conflict requires reset.");
  Serial.println("Verify exact breakout power/logic; SLNT must be LOW for TX. Engine-off validation first.");
  installCan(TWAI_MODE_LISTEN_ONLY);
}

void loop() {
  pollCan(); updateFlex(); readCommands();
  uint32_t now=millis();
  automaticStart(now); runTx(millis()); checkHealth(millis()); updateLed(millis()); report(millis());
  yield();
}




