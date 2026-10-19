# Progress

## Phase checklist

### Phase 0 — Repo setup
- [x] Docs skeleton (README, PROGRESS, CAN_PROTOCOL, HARDWARE)
- [x] Move `CAN_Flex_Slim_Ver5.ino` into `Holley_Canbus_info/CAN_Flex_Slim_Ver5/`
- [x] KiCad lib tables: absolute Windows paths changed to `${KIPRJMOD}/../Kicad-specific-libraries`
- [x] `.gitignore` for PlatformIO and KiCad backups
- [x] PlatformIO project scaffold with pinned platform

### Phase 1 — Firmware on a SuperMini dev board (same ESP32-C3 chip)
- [x] Flex input + device-1985 TX at parity with Slim Ver5 (Input1 Hz, Input2 ohms) — built, not yet bench-run
- [x] CAN RX: RPM + 4 configured Holley output rows (ECU serial learned from RPM frame); outputs OFF on stale/invalid — built, not yet bench-run
- [x] Outputs on/off via GPIO
- [ ] PWM outputs (LEDC, per-output frequency) — blocked on Bench B
- [x] ADS1115 driver + Input3-6 TX (dev board: breakout module) — built, not yet bench-run
- [x] NVS config: Holley row per output, WiFi password (ECU serial is learned from the RPM frame; PWM mode/frequency wait for Bench B)
- [x] WiFi web panel: status, AUTO/ALL OFF, 30 s heartbeat-guarded output test, TX start/stop, Bench A test volts, row + password settings — built, not yet run on hardware
- [x] Native unit tests: ID build, BE float, flex math, GM CTS curve (`pio test -e native`, 5 pass)
- [ ] **Bench A:** send fixed test values on Input3-6 candidate IDs; record what Holley EFI software shows
- [ ] **Bench B:** capture `IO_OUTPUT` frames while Holley drives a PWM output at 0/25/50/75/100 %; record float + status
- [ ] **Bench C:** unplug CAN with an output ON; confirm the module drops it within 500 ms and note how long Holley holds the last input value

### Phase 2 — Schematic
- [x] Replace SuperMini symbol with ESP32-C3-MINI-1
- [x] Power input, sensor 5 V, CAN, flex, analog, outputs, USB-C (flat A2 sheet, net labels)
- [x] JLC/LCSC part selection (LCSC field on every part except J1; table in HARDWARE.md)
- [x] ERC clean (0 errors, 0 warnings), no unconnected pins, no overlaps
- [x] Split into hierarchy: Power sheet (placeholder for user's own protection + 5V circuit) and Connector sheet (24 NC-flagged stubs)
- [ ] Draw power circuit on Power sheet
- [ ] Place harness connector on Connector sheet (delete NC flags, wire pins)
- [ ] Peer review of schematic with partner

### Phase 3 — PCB
- [ ] Connector part chosen and footprint verified
- [ ] Layout, power-ground routing for 2 output ground pins, quiet sensor ground
- [ ] DRC clean, JLC fab outputs (gerbers, BOM, CPL)

### Phase 4 — Bring-up and validation
- [ ] Power-up and current check, flash over USB-C
- [ ] Bench re-run of A/B/C on the real board
- [ ] Key-on engine-off in vehicle, then engine running

## Bench instructions (SuperMini + CAN Pal + ADS1115 breakout)
Wiring follows `firmware/src/pins.h`: CAN TX 4, RX 5, CAN Pal SLNT 21, flex 3 (4.7k pull-up to 3.3 V), ADS1115 SDA 8 / SCL 6, outputs 0/1/7/10 to LEDs or unloaded relays. Disconnect the Slim first: only one device 1985 may be on the bus.
- **Bench A:** serial `AINTEST 2.5`. In Holley EFI software, configure CAN device 1985 Input3-6 as 0-5 V and see whether 2.5 V appears. `AINTEST OFF` ends it.
- **Bench B:** serial `CAPTURE`. In Holley, set a row (e.g. row 5) as a PWM output and step the duty. Paste the `ROW n bits=... float=... status=...` lines into docs/CAN_PROTOCOL.md.
- **Bench C:** with an output ON, pull CANH/CANL. The output must drop within 0.5 s. Note how long Holley keeps showing the last flex value.

## Open questions
- Flash use is 83% of the default 1.3 MB app slot (WiFi stack). If OTA updates aren't wanted, a single-app partition table doubles the room. Decide before adding OTA.
- Output arming policy: Ver12 armed outputs only after all four rows had been OFF for 500 ms, and a CAN fault needed a manual re-ARM. The new firmware follows fresh Holley commands directly, so a fan or pump comes back on its own after a reset or CAN glitch. It keeps a 2 s boot guard and a serial DISARM latch. Confirm with partner.
- Connector part number and per-pin current rating (sets the max current per output).
- Identity of the "genuine device 1985" the bench work started from. Does Holley cap its input count?
- Highest PWM frequency the intended loads need (affects the output switch choice).

## Log
- 2026-10-19 — Plan agreed. MCU ESP32-C3-MINI-1, 4 low-side outputs (pin 24 changed to a second power GND), ADS1115 for analog, WiFi panel + USB-C. Phase 0 done.
- 2026-10-19 — Firmware scaffold: PlatformIO, pioarduino 55.03.311 pinned, no third-party libs. Flex/TX, RX/outputs, ADS1115, serial bench commands. `pio run` clean, native tests pass. Not yet run on hardware.
- 2026-10-19 — Web panel (web.h) + NVS settings (settings.h). Per-board random WiFi password replaces the hardcoded Ver12 one. Build clean, page JS syntax-checked, not yet run on hardware.
- 2026-10-19 — Schematic V001: ESP32-C3-MINI-1, LMR16006 buck, ME6211, TPS7B69 sensor 5 V, 4× BTS3256D, TJA1051T/3, ADS1115, USB-C. ERC clean. J1 footprint pending.
- 2026-10-19 — Schematic split into Root + Power + Connector sheets. Removed F1/D1/D2/LMR16006 buck and J1; Power sheet exposes VBAT_IN/GND in and VBAT/+5V out. ERC: 5 expected label_dangling errors until the placeholders are filled.
