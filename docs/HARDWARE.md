# Hardware

KiCad 10 project: `Sloppy_Holley_Flex-over-can/Flex-over-can/`. Fab and assembly: JLCPCB.

## Sheets
All sheets are US Letter. Nets that cross sheets use global labels; nets local to one sheet use local labels.

| Page | Sheet | File | Contents |
|-----:|-------|------|----------|
| 1 | Root | `Flex-over-can.kicad_sch` | Top level: sheet blocks, page index. Global labels tie the Power and Connector sheet pins to the other sheets. |
| 2 | Power | `Power.kicad_sch` | Placeholder for the user's own input protection + 5V circuit. In: `VBAT_IN`, `GND`. Out: `VBAT` (protected 12V, ~250 mA budget), `+5V` (size for 0.5 A). PWR_FLAGs on the outputs until the circuit is drawn. |
| 3 | Connector | `Connector.kicad_sch` | One stub per harness pin (1-24) ending in a no-connect flag. Place the chosen connector, delete each X, wire the pin to the stub. |
| 4 | 3V3 and Sensor 5V | `Supplies.kicad_sch` | U11 3V3 LDO, U3 sensor 5V, D4 USB VBUS OR-ing, GND PWR_FLAG |
| 5 | MCU ESP32-C3 | `MCU.kicad_sch` | U1, EN RC, BOOT button, status LED, I2C pull-ups |
| 6 | USB-C | `USB.kicad_sch` | J2, CC resistors, U9 ESD |
| 7 | CAN Bus | `CAN.kicad_sch` | U8 transceiver, JP1/R11 termination, D6 ESD |
| 8 | Flex Sensor Input | `Flex.kicad_sch` | F2 PTC, pull-up, series R, clamp |
| 9 | Analog Inputs | `Analog.kicad_sch` | 4x divider/RC/clamp, U10 ADS1115, NT1 star ground |
| 10 | Low-Side Outputs | `Outputs.kicad_sch` | U4-U7 BTS3256D with series, pulldown and SRP resistors |

Until both placeholders are filled, ERC reports 5 `label_dangling` errors on `VBAT_IN` and the Power sheet `GND`. They clear once a connector pin and the power circuit are on those nets.

## Block diagram

```
+12V (pin1) VBAT_IN -> [Power sheet: user's protection + 5V circuit] -> VBAT (protected 12V) + +5V
+5V -> LDO 3.3V -> ESP32-C3-MINI-1, ADS1115, CAN xcvr
VBAT -> protected sensor 5V -> pins 12/15/18/21
     -> polyfuse -> flex +12V (pin 5)
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

## Part choices (schematic V001, LCSC numbers are in each symbol's `LCSC` field)

| Ref | Part | LCSC | Notes |
|-----|------|------|-------|
| U1 | ESP32-C3-MINI-1-N4 | C2838502 | Espressif KiCad lib 3.2.1 vendored in `Kicad-specific-libraries/` |
| U11 | ME6211C33M5G-N LDO 3.3 V 0.5 A | C82942 | AP2112K symbol (same pinout) |
| U3 | TPS7B6950QDCYRQ1 sensor 5 V, 150 mA | C108469 | 40 V in, current limit + thermal shutdown |
| U4-U7 | BTS3256D HITFET low-side | C3232337 | 7.5 A nom, 10 mΩ, 3.3 V input, 12 kHz max PWM, 40 V active clamp, fault readback on IN |
| U8 | TJA1051T/3 CAN | C38695 | VIO 3.3 V, S pin pulled high (silent) |
| U10 | ADS1115IDGSR | C37593 | 0x48, 3.3 V |
| U9 | USBLC6-2SC6 | C7519 | USB ESD |
| D6 | NUP2105L CAN ESD | C284104 | |
| F2 | 0.2 A PTC | C69680 | flex sensor 12 V |
| J2 | USB-C HRO TYPE-C-31-M-12 | C165948 | |
| R38-R41 | 15k SRP (slew) | C22809 | DNP: open = fastest slew for PWM |

Passives are JLC Basic 0603 parts unless noted. Analog divider 12k/20k gives ×0.625, so firmware `ANALOG_SCALE` = 1.6 on this board.

## Design notes / open items
- Output inductive kickback is absorbed by the BTS3256D active clamp (40 V). That is fine for relays and low-frequency solenoids. High-frequency PWM of large inductive loads should get a flyback diode at the load. A diode on the board would short the battery through the HITFET body diode if the battery is connected backwards.
- Input protection and the 5V regulator are the user's own design on the Power sheet. VBAT must be reverse-polarity protected: it feeds the BTS3256D VS pins, U3 and F2. The earlier draft used F1 0.5 A PTC (C70077) -> SMBJ26CA TVS (C19077581) -> SS56 reverse diode (C18199169) -> LMR16006 buck (C87080, 22 µH, FB 56k/10k).
- Sensor 5 V shorted to 12 V can damage U3 (OUT abs max). Consider adding a TVS/series protection if the harness risk is real.
- ESP32 footprint's 3D model path uses `${KICAD8_3RD_PARTY}`; repoint to `3dmodels/ESP32-C3-MINI-1.STEP` during layout.

## Bring-up checklist
_Fill in during Phase 4._
