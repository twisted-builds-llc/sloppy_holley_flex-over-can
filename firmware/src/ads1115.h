#pragma once
// Minimal non-blocking ADS1115 driver: single-shot AINx vs GND, PGA +/-4.096 V, 860 SPS,
// round-robin over 4 channels. In-tree so no third-party library can change behavior.
#include <Arduino.h>
#include <Wire.h>

class Ads1115 {
public:
  static constexpr unsigned CHANNELS=4;
  static constexpr uint32_t STALE_MS=100;

  void begin(TwoWire &wire,uint8_t address=0x48){
    wire_=&wire;address_=address;channel_=0;startedAt_=millis();present_=start();
  }
  // Call every loop. Reads the finished conversion and starts the next channel.
  void tick(uint32_t now){
    if(!present_){
      if(now-startedAt_>=1000){startedAt_=now;present_=start();} // Retry a missing/failed chip.
      return;
    }
    if(now-startedAt_<2) return; // 860 SPS conversion takes ~1.2 ms.
    int16_t raw;
    if(!readRaw(raw)){present_=false;startedAt_=now;return;}
    volts_[channel_]=raw*(4.096f/32768.0f);seenAt_[channel_]=now;seen_[channel_]=true;
    channel_=(channel_+1)%CHANNELS;startedAt_=now;present_=start();
  }
  bool present() const {return present_;}
  bool fresh(unsigned ch,uint32_t now) const {return present_ && seen_[ch] && now-seenAt_[ch]<=STALE_MS;}
  float volts(unsigned ch) const {return volts_[ch];}

private:
  bool start(){
    // OS=1 start, MUX=1xx single-ended, PGA=001 (+/-4.096 V), MODE=1 single-shot, DR=111, comparator off.
    uint16_t config=0x8000|uint16_t(4+channel_)<<12|1u<<9|1u<<8|7u<<5|3u;
    wire_->beginTransmission(address_);
    wire_->write(0x01);wire_->write(uint8_t(config>>8));wire_->write(uint8_t(config));
    return wire_->endTransmission()==0;
  }
  bool readRaw(int16_t &raw){
    wire_->beginTransmission(address_);wire_->write(0x00);
    if(wire_->endTransmission(false)!=0 || wire_->requestFrom(address_,uint8_t(2))!=2) return false;
    uint8_t hi=wire_->read(),lo=wire_->read();
    raw=int16_t(uint16_t(hi)<<8|lo);return true;
  }
  TwoWire *wire_=nullptr;
  uint8_t address_=0x48,channel_=0;
  bool present_=false,seen_[CHANNELS]={};
  uint32_t startedAt_=0,seenAt_[CHANNELS]={};
  float volts_[CHANNELS]={};
};
