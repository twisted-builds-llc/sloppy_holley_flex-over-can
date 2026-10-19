# Sloppy Holley Flex-over-CAN

CAN bus input/output module for Holley Terminator X (also HP/Dominator class ECUs).

- **To Holley:** GM/Continental flex-fuel sensor (ethanol % + fuel temp) and four 0-5 V analog inputs, sent as CAN device 1985.
- **From Holley:** four low-side (ground-switching) outputs, each on/off or PWM, following Holley I/O output rows.
- ESP32-C3-MINI-1, WiFi AP web panel for status/config/test, USB-C for flashing.
- Built for JLCPCB assembly.

> Status: early development. See [docs/PROGRESS.md](docs/PROGRESS.md).
> Never use for engine control until the bench and engine-off validations in PROGRESS.md are complete.

## Connector (24 pin)

| Pin | Function | Pin | Function |
|----:|----------|----:|----------|
| 1 | +12 V in | 13 | Analog 1 signal |
| 2 | Power GND | 14 | Sensor GND |
| 3 | CAN H | 15 | +5 V sensor |
| 4 | CAN L | 16 | Analog 2 signal |
| 5 | Flex sensor +12 V out | 17 | Sensor GND |
| 6 | Flex sensor signal | 18 | +5 V sensor |
| 7 | Flex sensor GND | 19 | Analog 3 signal |
| 8 | Output 1 (low side) | 20 | Sensor GND |
| 9 | Output 2 (low side) | 21 | +5 V sensor |
| 10 | Output 3 (low side) | 22 | Analog 4 signal |
| 11 | Output 4 (low side) | 23 | Sensor GND |
| 12 | +5 V sensor | 24 | Power GND |

Output load current returns through pins 2 and 24. Sensor grounds are a separate quiet ground.

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
