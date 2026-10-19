# Hardware

KiCad 10 project: `Sloppy_Holley_Flex-over-can/Flex-over-can/`. Fab and assembly: JLCPCB.

## Block diagram

```
+12V (pin1) -> fuse -> reverse-polarity P-FET -> TVS -> buck 5V -> LDO 3.3V -> ESP32-C3-MINI-1, ADS1115, CAN xcvr
                                                    \-> protected sensor 5V -> pins 12/15/18/21
                                                    \-> polyfuse -> flex +12V (pin 5)
USB-C VBUS -> diode-OR into 5V (bench flashing without 12V)

Flex sig (pin6) -> pull-up 3.3V, series R, clamp, Schmitt -> GPIO3
AN1-4 (13/16/19/22) -> series R, clamp, divider, RC -> ADS1115 (I2C)
CAN H/L (3/4) -> ESD -> transceiver (3.3V I/O, silent pin) -> GPIO4/5, jumper 120R term
GPIO0/1/7/10 -> low-side switches -> pins 8-11; flyback to +12V; return via pins 2 + 24
```

## GPIO map (ESP32-C3-MINI-1) — draft

| GPIO | Use | Notes |
|-----:|-----|-------|
| 0 | OUT1 | gate/input pulldown holds OFF in reset |
| 1 | OUT2 | |
| 2 | Status LED (sink, LED to 3V3) | strap pin, must be high at boot; LED pull-up keeps it high |
| 3 | Flex signal in | same as Slim Ver5 |
| 4 | CAN TX | same as Slim Ver5 |
| 5 | CAN RX | same as Slim Ver5 |
| 6 | I2C SCL | |
| 7 | OUT3 | |
| 8 | I2C SDA | strap pin, must be high at boot; I2C pull-up keeps it high |
| 9 | BOOT button | strap |
| 10 | OUT4 | |
| 18/19 | USB D-/D+ | native USB-Serial/JTAG |
| 20 | spare | |
| 21 | CAN transceiver silent (S) | UART0 TX idles high = silent through boot; covers C3 TWAI errata |

The dev-board bench build (SuperMini) uses the same numbers where its pins allow. See `firmware/src/pins.h`.

## Part choices (to finalize in Phase 2, with LCSC numbers)

| Block | Candidate | LCSC | Notes |
|-------|-----------|------|-------|
| MCU | ESP32-C3-MINI-1 (4 MB) | TBD | antenna keep-out at board edge |
| ADC | ADS1115IDGSR | TBD | 3.3 V supply, PGA ±4.096 V |
| CAN xcvr | TJA1051T/3 or SN65HVD230 | TBD | 3.3 V I/O, silent pin |
| Output switch | protected low-side (HITFET/OmniFET class) or logic-level NMOS + TVS | TBD | ≥5 A cont., PWM-capable, 3.3 V input |
| Buck 12→5 V | TBD | TBD | ≥40 V input for load dump with TVS |
| LDO 3.3 V | TBD | TBD | ≥500 mA (WiFi peaks) |
| Sensor 5 V | protected LDO or current-limit switch | TBD | short to ground must not reset the MCU |
| USB ESD | USBLC6-2SC6 | TBD | |
| Connector | 24-pin, user choice | TBD | per-pin current rating sets output limit |

## Bring-up checklist
_Fill in during Phase 4._
