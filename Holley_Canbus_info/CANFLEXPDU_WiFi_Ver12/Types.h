#pragma once
#include <stdint.h>
struct IdRecord {
  uint32_t id,count,lastMs,changes;
  uint16_t dlcMask;
  uint8_t first[8],latest[8],lo[8],hi[8];
  uint8_t latestLen,firstLen,validMask,changedMask;
  bool used,ext,rtr;
};
struct Counters {
  uint32_t total,dataFrames,emptyFrames,remoteFrames,untracked,lastMs,lastId;
  bool moduleConflict;
  uint32_t polls;
  uint32_t rpmMs;
  float rpm;
  uint16_t unique;
  uint8_t rpmStatus;
  bool lastExt,rpmSeen,rpmValid;
  uint32_t outputMs[4],outputBits[4],outputStatus[4];
  bool outputSeen[4],outputValid[4],outputOn[4];
  bool flexSeen,flexValid,tempSeen,tempValid;
  uint32_t flexMs,tempMs;
  float flexHz,tempOhms;
};

constexpr bool validOutput(uint32_t bits,uint32_t status){
  return (bits==0 && status==42) || (bits==0x3F800000 && status==43);
}
constexpr bool freshOutput(bool seen,bool valid,uint32_t now,uint32_t last,uint32_t timeout){
  return seen && valid && uint32_t(now-last)<=timeout;
}
static_assert(validOutput(0,42), "Captured OFF must decode");
static_assert(validOutput(0x3F800000,43), "Captured ON must decode");
static_assert(!validOutput(0,43) && !validOutput(0x3F800000,42), "Conflicting states must fail");
static_assert(!validOutput(0x3F000000,43) && !validOutput(0x3F800000,0), "Analog/fault states must fail");
static_assert(freshOutput(true,true,500,0,500) && !freshOutput(true,true,501,0,500), "Timeout boundary");
static_assert(!freshOutput(false,true,1,0,500) && !freshOutput(true,false,1,0,500), "Missing/invalid must fail");
static_assert(freshOutput(true,true,100,0xFFFFFFF0,500), "Timestamp wrap must work");

constexpr bool autoArmReady(bool enabled,bool armed,bool allOff,uint32_t uptime,uint32_t dwell){
  return enabled && !armed && allOff && uptime>=2000 && dwell>=500;
}
static_assert(autoArmReady(true,false,true,2500,500), "Auto arm after stable OFF");
static_assert(!autoArmReady(true,false,true,2500,499), "Wait full OFF dwell");
static_assert(!autoArmReady(false,false,true,2500,500), "STOP must inhibit rearm");
static_assert(!autoArmReady(true,false,false,2500,500), "ON/invalid/stale cannot arm");
static_assert(!autoArmReady(true,false,true,1999,500), "Startup guard");
static_assert(!autoArmReady(true,true,true,2500,500), "Already armed");
