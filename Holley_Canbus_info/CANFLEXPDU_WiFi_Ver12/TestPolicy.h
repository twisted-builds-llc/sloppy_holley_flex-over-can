#pragma once
#include <stdint.h>
constexpr bool testExpired(uint32_t now,uint32_t since,uint32_t heartbeat,uint32_t maximum){
  return uint32_t(now-since)>=maximum || uint32_t(now-heartbeat)>=3000;
}
constexpr bool fauxMayStart(bool conflict,bool otherTest,bool active,bool safe,uint32_t uptime){
  return !conflict && !otherTest && !active && safe && uptime>=2000;
}
static_assert(!fauxMayStart(true,false,false,true,5000),"External1985 blocks faux");
static_assert(!fauxMayStart(false,true,false,true,5000),"Relay test blocks faux");
static_assert(!fauxMayStart(false,false,true,true,5000),"Active test cannot extend deadline");
static_assert(!fauxMayStart(false,false,false,false,5000),"Unsafe RPM/CAN/output state blocks faux");
static_assert(!fauxMayStart(false,false,false,true,1999),"Startup listen required");
static_assert(fauxMayStart(false,false,false,true,2000),"Valid bench start");
static_assert(!testExpired(59999,0,59999,60000),"Before deadline");
static_assert(testExpired(60000,0,60000,60000),"Hard60s limit even with fresh heartbeat");
static_assert(testExpired(3000,0,0,60000),"Heartbeat expires at3s");
static_assert(!testExpired(100,0xFFFFFF00,0xFFFFFFF0,60000),"Timers tolerate millis wrap");
