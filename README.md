# Sloppy Holley Flex-over-CAN

CAN bus input/output module for Holley Terminator X (also HP/Dominator class ECUs).

- **To Holley:** GM/Continental flex-fuel sensor (ethanol % + fuel temp) and four 0-5 V analog inputs, sent as CAN device 1985.
- **From Holley:** four low-side (ground-switching) outputs, each on/off or PWM, following Holley I/O output rows.
- ESP32-C3-MINI-1, WiFi AP web panel for status/config/test, USB-C for flashing.
- Built for JLCPCB assembly.

> Status: early development. See [docs/PROGRESS.md](docs/PROGRESS.md).
> Never use for engine control until the bench and engine-off validations in PROGRESS.md are complete.

## Connector (J1, HCCPHPE24BKA90F, 24 pin)

Cavity names are the J1 footprint pad names (rows A, B, C). Use them when building the harness.

| Cavity | Function | Cavity | Function | Cavity | Function |
|-------:|----------|-------:|----------|-------:|----------|
| A1 | +12 V in | B1 | Power GND | C1 | Output 3 (low side) |
| A2 | CAN H | B2 | +5 V sensor | C2 | +5 V sensor |
| A3 | CAN L | B3 | Analog 1 signal | C3 | Analog 3 signal |
| A4 | Flex sensor +12 V out | B4 | Sensor GND | C4 | Sensor GND |
| A5 | Flex sensor signal | B5 | +5 V sensor | C5 | +5 V sensor |
| A6 | Flex sensor GND | B6 | Analog 2 signal | C6 | Analog 4 signal |
| A7 | Power GND | B7 | Sensor GND | C7 | Sensor GND |
| A8 | Output 1 (low side) | B8 | Output 2 (low side) | C8 | Output 4 (low side) |

Output load current returns through A7 and B1. Sensor grounds (A6, B4, B7, C4, C7) are a separate quiet ground joined to power GND at one point on the board.

## Web panel

The module runs its own WiFi access point, `FLEXCAN-XXXX`. Each board makes a random password on first boot. Type `WIFI` on the USB serial console (115200) to see it. Then open http://192.168.4.1. The panel shows status and has controls for AUTO / ALL OFF, a 30 s output test, the Holley row for each output, and the WiFi password.

## Repository layout

| Path | Contents |
|------|----------|
| `docs/PROGRESS.md` | Phase checklist, work log, open questions |
| `docs/CAN_PROTOCOL.md` | Holley CAN IDs and encodings, marked verified or hypothesis |
| `docs/HARDWARE.md` | Block diagram, GPIO map, part choices |
| `firmware/` | PlatformIO project (VS Code) |
| `Sloppy_Holley_Flex-over-can/` | KiCad project and project-local libraries |
| `Holley_Canbus_info/` | Reference only: Holley CAN doc, DBC, and the two bench prototypes (`CANFLEXPDU_WiFi_Ver12`, `CAN_Flex_Slim_Ver5`) |

## Firmware build

Install the PlatformIO extension in VS Code, then open the `firmware/` folder. From a terminal:

```sh
cd firmware
pio run                 # build
pio run -t upload       # flash over USB-C
pio device monitor      # serial at 115200
pio test -e native      # host unit tests
```

The ESP32 platform is pinned to an exact release in `platformio.ini`. All code beyond the Arduino-ESP32 core lives in this repo, so a third-party library update cannot change the build.
