# Hardware

KiCad 10 project: `Sloppy_Holley_Flex-over-can/Flex-over-can/`. Fab and assembly: JLCPCB.

## Sheets
All sheets are US Letter. Nets that cross sheets use global labels; nets local to one sheet use local labels.

| Page | Sheet | File | Contents |
|-----:|-------|------|----------|
| 1 | Root | `Flex-over-can.kicad_sch` | Top level: sheet blocks, page index. Global labels tie the Power and Connector sheet pins to the other sheets. |
| 2 | Power | `Power.kicad_sch` | RV1 SMDJ24CA TVS, C1/C2, D1 B160 reverse diode (`VBAT_IN` -> `VBAT`), U2 L7805 5 V linear, C3/C4, D2 5.6 V zener clamp on `+5V` |
| 3 | Connector | `Connector.kicad_sch` | J1 HCCPHPE24BKA90F 24-pin harness connector |
| 4 | 3V3 and Sensor 5V | `Supplies.kicad_sch` | U11 3V3 LDO, U3 sensor 5V, D4 USB VBUS OR-ing, GND PWR_FLAG |
| 5 | MCU ESP32-C3 | `MCU.kicad_sch` | U1, EN RC, BOOT button, status LED, I2C pull-ups |
| 6 | USB-C | `USB.kicad_sch` | J2, CC resistors, U9 ESD |
| 7 | CAN Bus | `CAN.kicad_sch` | U8 transceiver, JP1/R11 termination, D6 ESD |
| 8 | Flex Sensor Input | `Flex.kicad_sch` | F2 PTC, pull-up, series R, clamp |
| 9 | Analog Inputs | `Analog.kicad_sch` | 4x divider/RC/clamp, U10 ADS1115, NT1 star ground |
| 10 | Low-Side Outputs | `Outputs.kicad_sch` | U4-U7 BTS3256D with series, pulldown and SRP resistors |

ERC: 0 errors, 1 warning (RV1 uses the `Device:Varistor_US` symbol, which differs from the current library copy; drawing only).

## Block diagram

```
+12V (J1 A1) VBAT_IN -> TVS -> B160 reverse diode -> VBAT (protected 12V) -> L7805 -> +5V
+5V -> LDO 3.3V -> ESP32-C3-MINI-1, ADS1115, CAN xcvr
VBAT -> protected sensor 5V -> B2/B5/C2/C5
     -> polyfuse -> flex +12V (A4)
USB-C VBUS -> diode-OR into 5V (bench flashing without 12V)

Flex sig (A5) -> 4.7k pull-up to 5V, 10k series, 1nF, BAT54S clamp to 3V3 -> GPIO3
AN1-4 (B3/B6/C3/C6) -> 12k/20k divider, 100nF, BAT54S clamp -> ADS1115 (I2C)
CAN H/L (A2/A3) -> ESD -> transceiver (3.3V I/O, silent pin) -> GPIO4/5, jumper 120R term
GPIO0/1/7/10 -> BTS3256D low-side switches -> A8/B8/C1/C8; active clamp; return via A7 + B1
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
| U2 | L7805ABD2T-TR 5 V linear, TO-263 | C86206 | about 1.5-2 W at 14 V in; tab is GND |
| D1 | B160-13-F reverse-polarity diode | C24784 | 1 A; carries VBAT loads only, not output load current |
| RV1 | SMDJ24CA TVS | C215903 | 24 V standoff, 38.9 V clamp at 77 A |
| D2 | 1SMB5919BT3G 5.6 V zener | C70260 | +5V overvoltage clamp |
| U1 | ESP32-C3-MINI-1-N4 | C2838502 | Espressif KiCad lib 3.2.1 vendored in `Kicad-specific-libraries/` |
| U11 | ME6211C33M5G-N LDO 3.3 V 0.5 A | C82942 | AP2112K symbol (same pinout) |
| U3 | TPS7B6950QDCYRQ1 sensor 5 V, 150 mA | C108469 | 40 V in, current limit + thermal shutdown |
| U4-U7 | BTS3256D HITFET low-side | C3232337 | 7.5 A nom, 10 mΩ, 3.3 V input, 12 kHz max PWM, 40 V active clamp, fault readback on IN |
| U8 | TJA1051T/3 CAN | C38695 | VIO 3.3 V, S pin pulled high (silent) |
| U10 | ADS1115IDGSR | C37593 | 0x48, 3.3 V |
| U9 | USBLC6-2SC6 | C7519 | USB ESD |
| D6 | NUP2105L CAN ESD | C284104 | |
| F2 | 0.2 A PTC | C69680 | flex sensor 12 V |
| J1 | HCCPHPE24BKA90F 24-pin | none | through-hole, no LCSC number: hand-solder or add for JLC THT assembly |
| J2 | USB-C HRO TYPE-C-31-M-12 | C165948 | |
| R38-R41 | 15k SRP (slew) | C22809 | DNP: open = fastest slew for PWM |

Passives are JLC Basic 0603 parts unless noted. Analog divider 12k/20k gives ×0.625, so firmware `ANALOG_SCALE` = 1.6 on this board.

## Design notes / open items
- Output inductive kickback is absorbed by the BTS3256D active clamp (40 V). That is fine for relays and low-frequency solenoids. High-frequency PWM of large inductive loads should get a flyback diode at the load. A diode on the board would short the battery through the HITFET body diode if the battery is connected backwards.
- Power input: RV1 TVS -> D1 reverse diode -> VBAT -> U2 L7805. VBAT feeds the BTS3256D VS pins, U2, U3 and F2. An earlier draft used an LMR16006 buck (C87080, 22 µH, FB 56k/10k); go back to a buck if U2 runs too hot.
- U2 thermal: about 1.5-2 W at 14 V in. In layout V001 the tab connects to top-layer GND pour only, with no thermal vias to the bottom pour. Marginal at engine-bay ambient.
- Load dump: RV1 clamps up to 38.9 V at full surge current. L7805 input abs max is 35 V. Fine with a load-dump-protected alternator, marginal without one.
- Output current: OUT1-4 traces are 1.0 mm on 1 oz copper, about 3 A continuous each (about 4 A at 20 °C rise), well below the BTS3256D 7.5 A rating. All return current goes through J1 A7 + B1; confirm the connector pin rating.
- CAN termination: JP1 ships open (no 120 Ω). Bridge it only if the harness is not already terminated.
- Sensor 5 V shorted to 12 V can damage U3 (OUT abs max). Consider adding a TVS/series protection if the harness risk is real.
- PCB V001: 98 × 79.5 mm, 2 layers, 1.6 mm, 1 oz. GND pour both sides; copper keep-out under the U1 antenna at the board edge. Routed with Freerouting (DSN/SES files are working files, not tracked).
- DRC: 0 unconnected. Remaining items are footprint-owned or cosmetic: J1 courtyard not closed (2 errors), J1 silk past board edge, C4 reference over U2 silk, U1/U3 differ from library copies, HOLE1/HOLE2 board-only.

## Bring-up checklist
_Fill in during Phase 4._
