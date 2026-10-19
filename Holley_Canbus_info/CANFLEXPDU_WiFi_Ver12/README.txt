CANFLEXPDU_WiFi_Ver12 -- live Slim summary + guarded faux flex/temp tests

FOUR-RELAY ESP32-WROOM BOARD. Original Slim firmware unchanged.
Board: ESP32 Dev Module, PSRAM Disabled, Arduino-ESP32 3.x (tested3.1.3).
Keep Types.h, WebPanel.h and TestPolicy.h alongside INO.
Libraries: Adafruit GFX, Adafruit ST7735 and ST7789, Adafruit BusIO.
WiFi/WebServer are included in ESP32 core. Serial115200 with newline.

NETWORK UNCHANGED
SSID CANFLEX-PDU / password CanFlex-Test11
Open http://192.168.4.1 -- stay connected despite no-internet warning.
No app/cloud/router or filesystem upload. Credentials shown on TFT/Serial.
WPA2 local AP, random per-boot POST token, no CORS. Page reload after board reset.
Any client with network password can control it; no individual users or TLS.
Maximum2 stations. Change password/source display labels before deployed use.

LIVE SLIM SUMMARY
With Slim connected, external device1985 Input1 shows ethanol and frequency.
Input2 resistance is converted back to approximate temperature using the same
GM-style curve used by Slim. It is not Holley's independently decoded display
and does not prove exact temperature calibration. Values expire after1 second
without matching frames, invalid payload, or CAN inactivity/fault.
DLC4 BE floats: 1E2027C1 frequency,1E2067C1 approximate GM CTS ohms.
Page identifies external readings separately from PDU synthetic outputs.

FAUX TEST -- SLIM MUST BE REMOVED FIRST
Defaults OFF at every boot, with no automatic start/resume or saved override.
To test the PDU faux source, disconnect Slim/genuine device1985 from CAN and
RESET PDU. Any observed source0x7C1 latches faux blocked until reset; its live
summary remains available. Do not bypass the guard or leave both broadcasters.

Keep ENGINE OFF and relay loads disconnected. Require healthy active CAN,
fresh valid RPM zero, all four Holley rows5-8 fresh valid OFF, and startup2s.
Choose HOLD percent0..100 and temperature-40..257F, or SWEEP20..40% and68..140F.
Sweep ignores hold fields; it reproduces the earlier20s/30s demo cycles.
Click ARM60s FAUX BROADCAST and accept bench reminder. Relays open and AUTO is
inhibited. This deliberately sends synthetic values into Holley device1985,
which Holley may display without a synthetic label. Never run engine in this mode.

Hard60s timer cannot be extended while active. Stops on3s browser heartbeat
loss, tab background/close STOP attempt, CAN fault, any competing source1985,
RPM nonzero/stale, Holley output ON/invalid/stale, transmit failure, or STOP.
Both relay-test and faux modes are mutually exclusive. Returning to AUTO ends
faux mode, clears pending frames, opens outputs, and waits all-OFF dwell.
No automatic restart after test expiration. No guessed sensor-fault frame.
Holley may hold previous values after TX stops; observe its actual timeout.

Faux frames (only while explicitly armed):
Announcement0E0027C1 DLC1 byte10 initially/every15.5s.
Input1 1E2027C1 DLC4 BE float percent+50Hz every100ms.
Input2 1E2067C1 DLC4 BE float approximate GM CTS resistance every100ms.
Normal arbitration/retry; pending queue beyond250ms or TX failure ends test.
PDU normal operation sends no application frames; normal CAN still ACKs traffic.

GM curve source: Electromotive TECgt TableG.3.3, printed page91:
https://electromotive.com/wp-content/uploads/2015/05/Tec-gt-Manual-2.3.pdf
This is approximate and NOT extracted Holley preset calibration.

RELAY CONTROLS RETAINED
AUTO follows Holley logical rows5,6,7,8; GPIO32,33,25,26 activeHIGH.
All four fresh valid OFF500ms required before auto-arm; boot2s guard retained.
Stale/invalid commands open affected output; CAN fault/receive loss disarms all.
Manual relay test:30s maximum,3s heartbeat, RPM zero/all Holley outputs OFF.
Expiry leaves outputs OFF. ALL OFF/DISARM and Serial STOP also end faux mode.
Serial TXSTART remains disabled: faux broadcasting is armed on web page only.
Commanded GPIO state is not measured contact/current feedback.

CAN TX17 RX16,1Mbps, ECU source150. Existing TFT wiring unchanged.
Confirmed Adafruit CAN Pal Vcc3.3V, common ground, CANH-H/CANL-L.
Only two physical bus-end terminators. Verify breakout power/logic, protected
power and hardware relay defaults; ESP32 GPIOs are not5V tolerant.

No ECU firmware/tune loading, MAP lock, or Slim settings writing implemented.
Independent lower-priority HTTP worker queues commands to main GPIO-control
loop; CAN RX task remains independent. Compile/page checks are not physical
Wi-Fi/relay timing validation. Perform engine-off unloaded tests first.
