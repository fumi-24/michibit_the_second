# Michi:bit the Second

A BLE-controlled Mars rover built on M5Stack Core2 + 4EncoderMotor, with a
Stackchan-style animated avatar face and an M5GO Bottom2 LED bar for visual
feedback. Controlled from a smartphone (or PC) via a custom TurboWarp
extension over Web Bluetooth.

Submitted to the **M5Stack Global Innovation Contest 2026**.

## Features

- Tank-style drive control (independent left/right wheel speed) plus a
  2-DOF arm, all over BLE
- Emergency stop, triggered from the phone, a physical button, or the touch
  screen
- Live telemetry (motor supply voltage/current) streamed over BLE Notify,
  viewable from a PC
- Animated avatar face (Stackchan-style) that reacts to driving and idles
  with its own personality when not in use
- Touch-screen "detail" mode with 8 directional buttons for manual control
  without a phone
- Per-channel speed limiting (drivetrain / arm 1 / arm 2)
- Automatic recovery: a dedicated watchdog reboots the board if I2C
  communication with the motor module ever locks up

## Hardware

- M5Stack Core2
- M5Stack 4EncoderMotor module (STM32 + BL5617 H-Bridge), stacked below
  Core2 (I2C: SDA=21, SCL=22, address 0x24)
- M5GO Bottom2 LED bar (SK6812 x10, GPIO25)
- Drive motors + arm motors wired to the 4EncoderMotor module

## Repository structure

```
michibit_the_second/          this repository
├── michibit_the_second/      Arduino sketch (flash this onto Core2)
│   └── michibit_the_second.ino
├── controller/                phone/PC-side controller (TurboWarp)
│   ├── ble-extension.js       custom Web Bluetooth extension for TurboWarp
│   ├── ble-turbowarp.sb3      TurboWarp project (phone controller)
│   ├── ble-turbowarp-pc.sb3   TurboWarp project (PC telemetry monitor)
│   └── ble-turbowarp.html     the phone controller, packaged as a standalone webpage
└── README.md
```

## Getting started

### 1. Flash the firmware

Open `michibit_the_second/michibit_the_second.ino` in the Arduino IDE and
flash it to a Core2 with the 4EncoderMotor module stacked underneath.
Required libraries: M5Unified, M5Stack_Avatar, M5Module-4EncoderMotor,
ArduinoJson, Adafruit_NeoPixel (see the file header for exact versions/links).

### 2. Run the controller

Either:

- Open `controller/ble-turbowarp.html` directly in a Web Bluetooth-capable
  browser (Chrome/Edge/Opera; HTTPS or localhost required), or
- Load `controller/ble-turbowarp.sb3` in [TurboWarp](https://turbowarp.org/)
  with `controller/ble-extension.js` loaded as a custom extension
  (**"Run without sandbox" must be enabled**, since the extension talks to
  the Web Bluetooth API directly).

`ble-turbowarp-pc.sb3` is a second project for a PC to connect separately
and monitor the live telemetry (voltage/current) stream.

## BLE protocol

The device advertises as `mars-core2` with a Nordic UART-style GATT service
exposing three characteristics:

| Characteristic | Property | Payload |
|---|---|---|
| Drive | Write | `{"left":-180..180,"right":-180..180}` |
| Arm | Write | `{"left":-180..180,"right":-180..180}` |
| Telemetry | Notify | `{"v":volts,"a":amps,"estop":0/1,"dl":..,"dr":..,"al":..,"ar":..}` sent every 300ms |

Sending `{"estop":1}` to either the Drive or Arm characteristic triggers an
emergency stop.

## License

This project's own code (`michibit_the_second.ino`, `ble-extension.js`, and
the TurboWarp projects) is released under the **MIT License**. See
[LICENSE](./LICENSE) for the full text.

### Third-party libraries and assets

This project depends on the following third-party libraries and assets,
each of which remains under its own original license (not MIT):

| Component | License | Source |
|---|---|---|
| M5Unified | MIT | https://github.com/m5stack/M5Unified |
| M5Stack_Avatar | MIT | https://github.com/meganetaaan/m5stack-avatar |
| M5Module-4EncoderMotor | MIT | https://github.com/m5stack/M5Module-4EncoderMotor |
| ArduinoJson | MIT | https://github.com/bblanchon/ArduinoJson |
| ESP32 BLE Library (BLEDevice/BLEServer/etc.) | Apache License 2.0 | part of [arduino-esp32](https://github.com/espressif/arduino-esp32) |
| arduino-esp32 (core framework) | LGPL-2.1 | https://github.com/espressif/arduino-esp32 |
| Adafruit NeoPixel | LGPL-3.0 | https://github.com/adafruit/Adafruit_NeoPixel |
| Icons used in the TurboWarp controller project | Apache License 2.0 | [Google Fonts Icons](https://fonts.google.com/icons) (Material Symbols) |

None of the above library source code is redistributed or modified in this
repository; they are pulled in via the Arduino Library Manager / Board
Manager at build time.

The animated face is inspired by the open-source
[Stack-chan](https://github.com/stack-chan/stack-chan) project. "スタックチャン"
/ "Stack-chan" is a trademark held defensively by the community to keep it
open for everyone to use; this project is an independent, unofficial build
and is not affiliated with or endorsed by the Stack-chan project.

## Author

fumi24
