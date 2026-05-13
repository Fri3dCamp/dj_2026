## Fri3d DJ Addon firmware

This firmware runs on the [CH32X035](https://www.wch-ic.com/products/CH32X035.html) MCU of the DJ addon board made for the [Fri3D Camp 2026 badge](https://github.com/Fri3dCamp/badge_2026).

The firmware handles different functions of the DJ addon and acts as a I2C IO expander to the Fri3D Camp 2026 badge:
 * Read the button matrix
 * Set the button RGB LEDs
 * Read all potmeters (6) and faders (3)
 * Read encoders (2)
 * Send MIDI CC messages over USB to a PC and over UART to the badge
 * Allow control from the badge using I2C

Please check the [schematics](TODO:local_link) to get more details about how these peripherals are connected to the CH32X035 MCU.

### UART

The DJ addon sends MIDI CC messages through the UART port to the badge at a baudrate of `115200 8N1`.

TODO: receive MIDI CC messages to control the LEDs.

### I2C

The DJ addon has I2C address `0x3A` and uses the following registers to interface/control with its connected peripherals:

| Address (hex) | Name | Access | Bytes | description |
|-|-|-|-|-|
| 0x00 | Version number | R | 3 | Reports the firmware version number |
| 0x03 | Button states | R | 1 | Reports the button states (see below) |
| 0x04 | Potmeter: left bottom | R | 2 | Reports the analog value (0-4095) |
| 0x06 | Potmeter: left middle | R | 2 | Reports the analog value (0-4095) |
| 0x08 | Potmeter: left top | R | 2 | Reports the analog value (0-4095) |
| 0x0a | Fader: left | R | 2 | Reports the analog value (0-4095) |
| 0x0c | Potmeter: right bottom | R | 2 | Reports the analog value (0-4095) |
| 0x0e | Potmeter: right middle | R | 2 | Reports the analog value (0-4095) |
| 0x10 | Potmeter: right top | R | 2 | Reports the analog value (0-4095) |
| 0x12 | Fader: right | R | 2 | Reports the analog value (0-4095) |
| 0x14 | Crossfader | R | 2 | Reports the analog value (0-4095) |
| 0x16 | Encoder: left | R | 2 | Reports the left encoder value (0-127) |
| 0x18 | Encoder: right | R | 2 | Reports the right encoder value (0-127) |
| 0x1A | Button RGB LEds | R/W | 24 | The RGB value of the button leds (0-255) |

## Building

Use [platformio](https://platformio.org) to build this project. You should install the [ch32v platform package](https://github.com/Community-PIO-CH32V/platform-ch32v) as well. If you use the command line, build using:

```
pio run
```

## Flashing

The easiest way to flash the DJ addon is using the USB port and a tool like [wchisp](https://github.com/ch32-rs/wchisp). First, disconnect the USB cable. While pressing the boot button on the board, reconnect the USB cable. Then run:

```
wchisp flash <path to the firmware.bin file>
```

TODO: Alternatively, the DJ addon can also be reflashed through the [Fri3D Camp 2026 badge](https://github.com/Fri3dCamp/badge_2026).
