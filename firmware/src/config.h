#pragma once
#include <stdint.h>
// Holley logical I/O rows that drive OUT1-4. A row is the line number in Holley's I/O setup,
// not an ECU pin. Rows 5-8 match the CANFLEXPDU Ver12 bench tune.
constexpr uint8_t OUTPUT_ROW[4]={5,6,7,8};
constexpr uint32_t OUTPUT_TIMEOUT_MS=500;  // Stale row command -> output OFF.
constexpr uint32_t BOOT_GUARD_MS=2000;     // Outputs stay OFF after reset.
// Connector-pin volts per ADS1115 volt. 1.0 for a bench breakout with no divider; 1.6 on the V001 board (12k/20k divider).
constexpr float ANALOG_SCALE=1.0f;
constexpr uint32_t TX_PERIOD_MS=100;       // Input frame rate, same as Slim Ver5.
constexpr uint32_t LISTEN_MS=1500;         // Silent listen for a competing device 1985 before TX.
constexpr uint32_t TX_RETRY_MS=3000;       // Back-off after a TX fault.
