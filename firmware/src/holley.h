#pragma once
// Holley EFI CAN encoding, sensor math and output policy. Pure C++ (no Arduino) so the
// native unit tests can include it. IDs and their verified/hypothesis status: docs/CAN_PROTOCOL.md.
#include <stdint.h>
#include <string.h>
#include <math.h>

namespace holley {

// ---- IDs ----
// This module is CAN device 1985: source ID 4, serial 0x7C1. Low 14 bits of every frame we send.
constexpr uint32_t DEVICE_SERIAL=0x7C1;
constexpr uint32_t DEVICE_LOW=(4u<<11)|DEVICE_SERIAL;
constexpr uint32_t ANNOUNCE_ID=0x0E000000|DEVICE_LOW;
constexpr uint8_t ANNOUNCE_BYTE=0x10;
constexpr uint32_t ANNOUNCE_MS=15500;
// Input n (1-based) is channel 127+n. Inputs 1-2 verified; 3-6 are the Bench A hypothesis.
constexpr uint32_t inputId(unsigned n){return 0x1E000000|(uint32_t(127+n)<<14)|DEVICE_LOW;}
static_assert(ANNOUNCE_ID==0x0E0027C1 && inputId(1)==0x1E2027C1 && inputId(2)==0x1E2067C1, "Verified device 1985 IDs");

// ECU frames carry source ID 2 and the ECU serial in the low 11 bits.
constexpr uint32_t ECU_SOURCE=2u<<11;
constexpr uint32_t RPM_BASE=0x1E005000;
constexpr uint32_t rpmId(uint32_t ecu){return RPM_BASE|ecu;}
constexpr uint32_t pollId(uint32_t ecu){return 0x0E005000|ecu;}
// IO_OUTPUT1..62 are channels 378..439.
constexpr unsigned OUTPUT_ROWS=62, OUTPUT1_CHANNEL=378;
constexpr uint32_t outputRowId(unsigned row,uint32_t ecu){
  return 0x1E000000|(uint32_t(OUTPUT1_CHANNEL-1+row)<<14)|ECU_SOURCE|ecu;
}
// Row number (1..62) of an IO_OUTPUT monitor frame from this ECU, or 0.
constexpr unsigned outputRow(uint32_t id,uint32_t ecu){
  return (id>>25)==0xF && (id&0x3FFF)==(ECU_SOURCE|ecu) &&
         ((id>>14)&0x7FF)>=OUTPUT1_CHANNEL && ((id>>14)&0x7FF)<OUTPUT1_CHANNEL+OUTPUT_ROWS
         ? ((id>>14)&0x7FF)-(OUTPUT1_CHANNEL-1) : 0;
}
static_assert(rpmId(0x150)==0x1E005150 && pollId(0x150)==0x0E005150, "Verified ECU IDs");
static_assert(outputRowId(1,0x150)==0x1E5E9150 && outputRowId(5,0x150)==0x1E5F9150, "DBC IO_OUTPUT1, Ver12 row 5");
static_assert(outputRow(0x1E5F9150,0x150)==5 && outputRow(0x1E5F9151,0x150)==0 && outputRow(rpmId(0x150),0x150)==0, "Row decode");

// ---- Big-endian payloads ----
inline uint32_t getU32BE(const uint8_t *d){return uint32_t(d[0])<<24|uint32_t(d[1])<<16|uint32_t(d[2])<<8|d[3];}
inline float bitsToFloat(uint32_t bits){float v;memcpy(&v,&bits,4);return v;}
inline void putFloatBE(float v,uint8_t *d){
  static_assert(sizeof(float)==4,"32-bit float required");
  uint32_t b;memcpy(&b,&v,4);d[0]=uint8_t(b>>24);d[1]=uint8_t(b>>16);d[2]=uint8_t(b>>8);d[3]=uint8_t(b);
}

// ---- Output policy (from CANFLEXPDU Ver12 Types.h) ----
// Captured OFF = float 0 + status 42; ON = float 1.0 + status 43. PWM encoding unknown until Bench B.
constexpr bool validOutput(uint32_t bits,uint32_t status){
  return (bits==0 && status==42) || (bits==0x3F800000 && status==43);
}
constexpr bool freshOutput(bool seen,bool valid,uint32_t now,uint32_t last,uint32_t timeout){
  return seen && valid && uint32_t(now-last)<=timeout;
}
static_assert(validOutput(0,42) && validOutput(0x3F800000,43), "Captured states must decode");
static_assert(!validOutput(0,43) && !validOutput(0x3F800000,42), "Conflicting states must fail");
static_assert(!validOutput(0x3F000000,43) && !validOutput(0x3F800000,0), "Analog/fault states must fail");
static_assert(freshOutput(true,true,500,0,500) && !freshOutput(true,true,501,0,500), "Timeout boundary");
static_assert(!freshOutput(false,true,1,0,500) && !freshOutput(true,false,1,0,500), "Missing/invalid must fail");
static_assert(freshOutput(true,true,100,0xFFFFFFF0,500), "Timestamp wrap must work");

// ---- Manual output test (from CANFLEXPDU Ver12 TestPolicy.h) ----
constexpr uint32_t TEST_MAX_MS=30000, HEARTBEAT_MS=3000;
constexpr bool testExpired(uint32_t now,uint32_t since,uint32_t heartbeat,uint32_t maximum){
  return uint32_t(now-since)>=maximum || uint32_t(now-heartbeat)>=HEARTBEAT_MS;
}
static_assert(!testExpired(29999,0,29999,30000) && testExpired(30000,0,30000,30000), "Hard limit even with fresh heartbeat");
static_assert(testExpired(3000,0,0,30000), "Heartbeat expires at 3s");
static_assert(!testExpired(100,0xFFFFFF00,0xFFFFFFF0,30000), "Timers tolerate millis wrap");

// ---- Flex sensor (from CAN_Flex_Slim_Ver5; provisional Continental-style conversion) ----
constexpr float FLEX_MIN_HZ=49.5f, FLEX_MAX_HZ=150.5f; // Endpoint jitter allowance around 50..150 Hz.
inline bool flexFrequencyOk(float hz){return hz>=FLEX_MIN_HZ && hz<=FLEX_MAX_HZ;}
inline bool flexWidthOk(uint32_t lowUs,uint32_t periodUs){return lowUs<periodUs && lowUs>=1000 && lowUs<=5000;}
inline float ethanolPercent(float hz){float e=hz-50.0f;return e<0?0:e>100?100:e;}
inline float flexInputHz(float hz){return hz<50?50:hz>150?150:hz;}  // Value sent on Input1.
inline float flexTempC(float lowMs){return 41.25f*lowMs-81.25f;}

// Approximate GM-style CTS curve, Electromotive TECgt manual Table G.3.3, p91.
// Not an extracted Holley table. Sent on Input2 as equivalent ohms.
inline float gmCtsOhms(float c){
  static const float temperatures[]={-40,-18,-7,4,20,38,70,100,110,120,140};
  static const float resistances[]={100700,25000,13500,7500,3400,1800,450,185,133,102,62};
  constexpr size_t count=sizeof(temperatures)/sizeof(temperatures[0]);
  if(c<=temperatures[0]) return resistances[0];
  if(c>=temperatures[count-1]) return resistances[count-1];
  for(size_t i=1;i<count;++i) if(c<=temperatures[i]){
    // Interpolate ln(R) versus reciprocal Kelvin for thermistor behavior.
    float k=1.0f/(c+273.15f), k0=1.0f/(temperatures[i-1]+273.15f), k1=1.0f/(temperatures[i]+273.15f);
    float f=(k-k0)/(k1-k0);
    return expf(logf(resistances[i-1])+f*(logf(resistances[i])-logf(resistances[i-1])));
  }
  return resistances[count-1];
}

} // namespace holley
