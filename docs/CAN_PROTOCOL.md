# Holley CAN protocol notes

Sources: `Holley_Canbus_info/Holley EFI CAN Document.pdf`, `HP Dominator & Terminator X DBC.dbc`, and bench captures from the two prototypes in `Holley_Canbus_info/`.

Each item is tagged **VERIFIED** (seen working on the bench Terminator X MAX, firmware 3.1) or **HYPOTHESIS** (still needs a bench test).

## Bus
- 1 Mbit/s, 29-bit extended IDs. **VERIFIED**
- ID bit layout (Holley doc):

| Bits | Field |
|------|-------|
| 28 | Command bit (1 for monitor/data frames) |
| 27:25 | Target ID (7 = broadcast) |
| 24:14 | Target serial, used as channel index |
| 13:11 | Source ID (2 = ECU) |
| 10:0 | Source serial (low 11 bits of device serial) |

- Values are a big-endian IEEE-754 float. ECU monitor frames are DLC 8: float value (bytes 0-3) + u32 status (bytes 4-7).

## ECU -> module (read)

ECU serial on the bench ECU is `0x150`. Firmware must make it configurable or learn it from the RPM frame.

| Item | ID | Notes | Status |
|------|----|-------|--------|
| RPM | `0x1E005000 \| serial` | float RPM; byte 7 status (0 OK, 32 stall, 33 synced, 34 syncing) | VERIFIED |
| Device poll | `0x0E005000 \| serial`, DLC 0 | Module replies with its announcement | VERIFIED |
| IO_OUTPUT row n | `0x1E5E9000 + (n-1)*0x4000 \| serial`, n = 1..62 | OFF: float 0.0 + status 42. ON: float 1.0 (`0x3F800000`) + status 43 | VERIFIED (on/off) |
| IO_OUTPUT PWM | same | Duty encoding unknown | HYPOTHESIS, Bench B |

"Row" means the logical row in Holley's I/O configuration, not the ECU pin number. Rows 5-8 were used on the bench.

Holley does not broadcast PWM frequency. The module sets frequency per output from its own config.

## Module -> ECU (send) — CAN device 1985

Source ID 4, serial `0x7C1` (1985). Low 14 bits of every ID = `0x27C1`.

| Item | ID | Payload | Rate | Status |
|------|----|---------|------|--------|
| Announcement | `0x0E0027C1` | DLC 1, `0x10` | every 15.5 s and on ECU poll | VERIFIED |
| Input1 (ch 128) | `0x1E2027C1` | DLC 4 BE float, flex frequency Hz (ethanol % + 50) | 100 ms | VERIFIED |
| Input2 (ch 129) | `0x1E2067C1` | DLC 4 BE float, GM-CTS-equivalent ohms (fuel temp) | 100 ms | VERIFIED |
| Input3 (ch 130) | `0x1E20A7C1` | DLC 4 BE float, volts | 100 ms | HYPOTHESIS, Bench A |
| Input4 (ch 131) | `0x1E20E7C1` | same | 100 ms | HYPOTHESIS |
| Input5 (ch 132) | `0x1E2127C1` | same | 100 ms | HYPOTHESIS |
| Input6 (ch 133) | `0x1E2167C1` | same | 100 ms | HYPOTHESIS |

General form: `0x1E000000 | (channel << 14) | 0x27C1`, where channel = 127 + input number.

Notes:
- The GM CTS curve in the firmware is approximate (Electromotive TECgt manual table G.3.3), not taken from Holley's preset.
- Only one device may use serial 0x7C1. Firmware latches a conflict and stops TX if another source with low 11 bits 0x7C1 is seen.
- Holley may hold the last value after TX stops. Measure its timeout (Bench C).

## Bench results
_Record captures and outcomes here with dates._
