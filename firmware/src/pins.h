#pragma once
#include <stdint.h>
// ESP32-C3 GPIO map. Matches docs/HARDWARE.md; flex and CAN pins match CAN_Flex_Slim_Ver5.
constexpr uint8_t PIN_OUT[4]={0,1,7,10};   // Low-side outputs, active HIGH. Board pulldowns hold OFF through reset.
constexpr uint8_t PIN_LED=2;               // Status LED, active LOW (strap pin must idle high).
constexpr uint8_t PIN_FLEX=3;              // Flex signal, open collector with pull-up to 3.3V.
constexpr uint8_t PIN_CAN_TX=4, PIN_CAN_RX=5;
constexpr uint8_t PIN_SCL=6, PIN_SDA=8;    // ADS1115. SDA on strap GPIO8: I2C pull-up keeps it high at boot.
constexpr uint8_t PIN_CAN_SILENT=21;       // Transceiver S/SLNT: HIGH = silent (no TX on the wire).
