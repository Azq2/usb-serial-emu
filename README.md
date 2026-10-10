# USB serial emulator for RP2040

Firmware for Raspberry Pi Pico that emulates USB-to-serial adapters. Linux uses its standard drivers. Data written to the serial port is echoed back.

## Profiles

| Profile | USB ID | Linux driver |
|---|---|---|
| `ft232r` | `0403:6001` | `ftdi_sio` |
| `pl2303-h` | `067b:2303` | `pl2303` |
| `pl2303-hx` | `067b:2303` | `pl2303` |
| `pl2303-ta` | `067b:2303` | `pl2303` |
| `pl2303-tb` | `067b:2303` | `pl2303` |
| `pl2303-hxd` | `067b:2303` | `pl2303` |
| `pl2303-hxd-clone` | `067b:2303` | `pl2303` |
| `pl2303-hxn` | `067b:2303` | `pl2303` |
| `ch340` | `1a86:7523` | `ch341` |
| `cp2102` | `10c4:ea60` | `cp210x` |
| `cdc-acm` | `2e8a:000a` | `cdc_acm` |
| `siemens-sx1` | `11f5:0001` | `pl2303` |
| `siemens-x65` | `11f5:0003` | `pl2303` |
| `siemens-x75` | `11f5:0004` | `pl2303` |
| `siemens-ef81` | `11f5:0005` | `pl2303` |
| `benq-siemens-s81` | `04a5:4027` | `pl2303` |

## Build

Install CMake, the Arm GNU Toolchain, and `picotool`, then run:

```sh
git submodule update --init lib/pico-sdk lib/cherryusb
cmake -S . -B build -DPICO_BOARD=pico
cmake --build build
picotool load -v -x build/usb_serial_emu.uf2
```

Without a saved profile, the firmware starts as `ft232r`.

## Switch profiles

Pass the current serial device and the new profile:

```sh
./tools/usbemu-switch.py /dev/ttyUSB1 select cp2102
```

The device disconnects and appears again with a new USB ID. The selected profile is lost after a power cycle. Use `--persist` to save it:

```sh
./tools/usbemu-switch.py /dev/ttyUSB1 select cdc-acm --persist
```

## BOOTSEL and flashing

```sh
./tools/usbemu-switch.py /dev/ttyUSB1 bootsel
./tools/usbemu-switch.py /dev/ttyUSB1 flash build/usb_serial_emu.uf2
```

## Test

The test switches through every profile and checks the USB ID, Linux driver, control lines, BREAK, and loopback data:

```sh
./tools/verify-hardware.py /dev/ttyUSB1
./tools/verify-hardware.py /dev/ttyUSB1 --stress-seconds 10 --stress-rate 90000
```

The firmware emulates the USB protocol and serial settings. It does not generate UART signals or reproduce hardware timing.
