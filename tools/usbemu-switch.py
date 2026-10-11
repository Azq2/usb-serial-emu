#!/usr/bin/env python3
"""Enter RP2040 BOOTSEL through an emulated serial port and optionally flash."""

import argparse
import fcntl
import os
from pathlib import Path
import struct
import subprocess
import sys
import termios
import time


TIOCSBRK = 0x5427

PROFILE_BAUD = {
    "ft232r": 600,
    "pl2303-h": 1200,
    "pl2303-hx": 1800,
    "pl2303-ta": 2400,
    "pl2303-tb": 4800,
    "pl2303-hxd": 9600,
    "pl2303-hxd-clone": 19200,
    "pl2303-hxn": 38400,
    "ch340": 7200,
    "cp2102": 14400,
    "cdc-acm": 28800,
    "siemens-sx1": 57600,
    "siemens-x65": 115200,
    "siemens-x75": 230400,
    "siemens-ef81": 460800,
    "benq-siemens-s81": 921600,
    "cdc-test": 9600,
}


def send_management_command(device: str, baud: int,
                            persistent: bool = False,
                            even_parity: bool = False) -> None:
    fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_CLOEXEC)
    try:
        attrs = termios.tcgetattr(fd)
        attrs[0] = 0
        attrs[1] = 0
        attrs[2] &= ~(termios.CSIZE | termios.PARENB | termios.PARODD)
        attrs[2] |= (termios.CS7 | termios.PARENB | termios.CSTOPB |
                     termios.CLOCAL | termios.CREAD)
        if not even_parity:
            attrs[2] |= termios.PARODD
        attrs[3] = 0
        speed = getattr(termios, f"B{baud}")
        attrs[4] = speed
        attrs[5] = speed
        termios.tcsetattr(fd, termios.TCSANOW, attrs)

        dtr_operation = (termios.TIOCMBIS if persistent else
                         termios.TIOCMBIC)
        fcntl.ioctl(fd, dtr_operation, struct.pack("I", termios.TIOCM_DTR))
        fcntl.ioctl(fd, termios.TIOCMBIS,
                    struct.pack("I", termios.TIOCM_RTS))
        fcntl.ioctl(fd, TIOCSBRK)
    except OSError as error:
        # Disconnect during the final ioctl means the command succeeded.
        # PL2303 HXD clones advertise the Linux NO_BREAK_GETLINE quirk.  Their
        # driver returns ENOTTY without sending BREAK; the guarded line format,
        # modem state, and service baud have already armed the command.
        if error.errno not in (5, 19, 25):
            raise
    finally:
        os.close(fd)


def wait_for_bootsel(timeout: float = 8.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = subprocess.run(
            ["picotool", "info"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        if result.returncode == 0:
            return
        time.sleep(0.1)
    raise TimeoutError("RP2040 did not appear in BOOTSEL mode")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("device", help="current emulator tty, e.g. /dev/ttyUSB1")
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("bootsel", help="enter ROM USB boot mode only")
    select_parser = subparsers.add_parser(
        "select", help="select a chip profile and re-enumerate")
    select_parser.add_argument("profile", choices=PROFILE_BAUD)
    select_parser.add_argument(
        "--persist", action="store_true",
        help="save the power-on fallback in flash (default: volatile scratch)")
    flash_parser = subparsers.add_parser("flash", help="enter BOOTSEL and load UF2")
    flash_parser.add_argument("uf2", type=Path)
    args = parser.parse_args()

    if args.command == "flash" and not args.uf2.is_file():
        parser.error(f"UF2 does not exist: {args.uf2}")

    if args.command == "select":
        send_management_command(args.device, PROFILE_BAUD[args.profile],
                                args.persist,
                                even_parity=args.profile == "cdc-test")
        storage = "flash" if args.persist else "watchdog scratch"
        print(f"selected {args.profile} via {storage}; "
              "device is re-enumerating")
        return 0

    send_management_command(args.device, 300)
    wait_for_bootsel()
    print("RP2040 is in BOOTSEL mode")

    if args.command == "flash":
        return subprocess.run(
            ["picotool", "load", "-v", "-x", str(args.uf2)],
            check=False,
        ).returncode
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, TimeoutError) as error:
        print(f"usbemu-switch: {error}", file=sys.stderr)
        raise SystemExit(1)
