#!/usr/bin/env python3
"""Cycle every runtime profile and verify Linux binding and raw loopback."""

import argparse
import errno
import fcntl
import os
from pathlib import Path
import select
import struct
import subprocess
import sys
import termios
import time
import tty


ROOT = Path(__file__).resolve().parent.parent
SWITCH_PATH = ROOT / "tools" / "usbemu-switch.py"

TIOCSBRK = 0x5427
TIOCCBRK = 0x5428
IO_TIMEOUT = 5.0

BOUNDARY_SIZES = (
    1, 8, 9,
    31, 32, 33,
    61, 62, 63, 64, 65,
    127, 128, 129,
    255, 256, 257,
    1023, 1024, 1025,
)

PROFILES = {
    "ft232r": ("0403", "6001", "0600", "ftdi_sio"),
    "pl2303-h": ("067b", "2303", "0100", "pl2303"),
    "pl2303-hx": ("067b", "2303", "0300", "pl2303"),
    "pl2303-ta": ("067b", "2303", "0300", "pl2303"),
    "pl2303-tb": ("067b", "2303", "0500", "pl2303"),
    "pl2303-hxd": ("067b", "2303", "0400", "pl2303"),
    "pl2303-hxd-clone": ("067b", "2303", "0400", "pl2303"),
    "pl2303-hxn": ("067b", "2303", "0100", "pl2303"),
    "ch340": ("1a86", "7523", "0264", "ch341-uart"),
    "cp2102": ("10c4", "ea60", "0100", "cp210x"),
    "cdc-acm": ("2e8a", "000a", "0100", "cdc_acm"),
    "siemens-sx1": ("11f5", "0001", "0300", "pl2303"),
    "siemens-x65": ("11f5", "0003", "0300", "pl2303"),
    "siemens-x75": ("11f5", "0004", "0300", "pl2303"),
    "siemens-ef81": ("11f5", "0005", "0300", "pl2303"),
    "benq-siemens-s81": ("04a5", "4027", "0300", "pl2303"),
}

SIEMENS_PROFILES = {
    "siemens-sx1",
    "siemens-x65",
    "siemens-x75",
    "siemens-ef81",
    "benq-siemens-s81",
}


def read_text(path: Path) -> str:
    return path.read_text(encoding="ascii").strip().lower()


def usb_device_for_tty(tty_path: Path) -> Path:
    current = (Path("/sys/class/tty") / tty_path.name / "device").resolve()
    for candidate in (current, *current.parents):
        if (candidate / "idVendor").is_file():
            return candidate
    raise RuntimeError(f"cannot find USB parent for {tty_path}")


def tty_on_usb_device(usb_device: Path, expected: tuple[str, str, str, str],
                      previous_devnum: str | None = None,
                      timeout: float = 8.0) -> Path:
    vid, pid, bcd_device, expected_driver = expected
    deadline = time.monotonic() + timeout
    last_error = "device did not appear"
    while time.monotonic() < deadline:
        for pattern in ("ttyUSB*", "ttyACM*"):
            for entry in Path("/sys/class/tty").glob(pattern):
                device_node = Path("/dev") / entry.name
                try:
                    candidate = usb_device_for_tty(device_node)
                    if candidate.resolve() != usb_device.resolve():
                        continue
                    if (previous_devnum is not None and
                            read_text(candidate / "devnum") ==
                            previous_devnum):
                        last_error = "device has not re-enumerated yet"
                        continue
                    actual = (
                        read_text(candidate / "idVendor"),
                        read_text(candidate / "idProduct"),
                        read_text(candidate / "bcdDevice"),
                    )
                    driver = (entry / "device" / "driver").resolve().name
                    if actual != (vid, pid, bcd_device):
                        last_error = f"identity {actual}, expected {(vid, pid, bcd_device)}"
                        continue
                    if driver != expected_driver:
                        last_error = f"driver {driver}, expected {expected_driver}"
                        continue
                    # The tty class node can appear before udev has applied its
                    # final owner/mode.  Do not hand a transient root-only node
                    # to the test (or to the next profile switch).
                    try:
                        probe_fd = os.open(
                            device_node,
                            os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK |
                            os.O_CLOEXEC)
                    except PermissionError:
                        last_error = f"{device_node} is not accessible yet"
                        continue
                    else:
                        os.close(probe_fd)
                    return device_node
                except (FileNotFoundError, RuntimeError):
                    continue
        time.sleep(0.05)
    raise TimeoutError(last_error)


def set_modem(fd: int, operation: int, bits: int) -> None:
    fcntl.ioctl(fd, operation, struct.pack("I", bits))


def read_exact(fd: int, size: int, timeout: float = IO_TIMEOUT) -> bytes:
    result = bytearray()
    deadline = time.monotonic() + timeout
    while len(result) < size:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        readable, _, _ = select.select([fd], [], [], min(remaining, 0.2))
        if readable:
            try:
                chunk = os.read(fd, size - len(result))
            except BlockingIOError:
                continue
            if not chunk:
                break
            result.extend(chunk)
    return bytes(result)


def write_all(fd: int, data: bytes, profile: str,
              timeout: float = IO_TIMEOUT) -> None:
    offset = 0
    deadline = time.monotonic() + timeout
    while offset < len(data):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(
                f"{profile}: write timed out with "
                f"{len(data) - offset} bytes pending")
        _, writable, _ = select.select([], [fd], [], min(remaining, 0.2))
        if not writable:
            continue
        try:
            count = os.write(fd, data[offset:])
        except BlockingIOError:
            continue
        if count == 0:
            raise OSError(f"{profile}: serial port accepted a zero-length write")
        offset += count


def verify_data(profile: str, expected: bytes, actual: bytes) -> None:
    common = min(len(expected), len(actual))
    for index in range(common):
        if actual[index] != expected[index]:
            raise RuntimeError(
                f"{profile}: {len(expected)}-byte test differs at offset {index}: "
                f"got 0x{actual[index]:02x}, expected 0x{expected[index]:02x}")
    if len(actual) != len(expected):
        raise RuntimeError(
            f"{profile}: {len(expected)}-byte test returned "
            f"{len(actual)} bytes")


def configure_tty(fd: int, profile: str) -> None:
    tty.setraw(fd)
    attrs = termios.tcgetattr(fd)
    attrs[2] &= ~(termios.CSIZE | termios.PARENB | termios.PARODD |
                  termios.CSTOPB | getattr(termios, "CRTSCTS", 0))
    attrs[2] |= termios.CS8 | termios.CLOCAL | termios.CREAD
    attrs[4] = termios.B115200
    attrs[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)

    set_modem(fd, termios.TIOCMBIC, termios.TIOCM_DTR | termios.TIOCM_RTS)
    set_modem(fd, termios.TIOCMBIS, termios.TIOCM_DTR | termios.TIOCM_RTS)
    try:
        fcntl.ioctl(fd, TIOCSBRK)
        fcntl.ioctl(fd, TIOCCBRK)
    except OSError as error:
        if profile != "pl2303-hxd-clone" or error.errno != errno.ENOTTY:
            raise


def verify_modem_inputs(fd: int, profile: str) -> None:
    if profile not in SIEMENS_PROFILES:
        return

    expected = termios.TIOCM_CTS | termios.TIOCM_DSR | termios.TIOCM_CAR
    deadline = time.monotonic() + 1.0
    while True:
        packed = fcntl.ioctl(fd, termios.TIOCMGET, struct.pack("I", 0))
        actual = struct.unpack("I", packed)[0]
        if actual & expected == expected:
            return
        if time.monotonic() >= deadline:
            raise RuntimeError(
                f"{profile}: modem status 0x{actual:x}, "
                f"expected mask 0x{expected:x}")
        time.sleep(0.01)


def verify_loopback(device: Path, profile: str) -> None:
    fd = os.open(
        device,
        os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK | os.O_CLOEXEC,
    )
    try:
        configure_tty(fd, profile)
        verify_modem_inputs(fd, profile)

        for size in BOUNDARY_SIZES:
            payload = pattern_slice(size << 12, size)
            write_all(fd, payload, profile)
            received = read_exact(fd, size)
            verify_data(profile, payload, received)
    finally:
        os.close(fd)


def pattern_slice(offset: int, size: int) -> bytes:
    return bytes(
        (position * 37 + (position >> 8) * 17 +
         (position >> 16) * 29 + (position >> 24) * 43 + 0x5a) & 0xff
        for position in range(offset, offset + size)
    )


def stress_loopback(device: Path, profile: str, seconds: float,
                    window: int = 65536,
                    rate: int = 0) -> tuple[int, float]:
    fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK |
                 os.O_CLOEXEC)
    started = time.monotonic()
    deadline = started + seconds
    drain_deadline = deadline + max(10.0, seconds)
    written = 0
    received = 0
    try:
        configure_tty(fd, profile)
        while time.monotonic() < deadline or received < written:
            now = time.monotonic()
            producing = now < deadline
            if not producing and now >= drain_deadline:
                raise TimeoutError(
                    f"{profile}: drain timed out with "
                    f"{written - received} bytes pending")

            read_set = [fd] if received < written else []
            rate_budget = (window if rate <= 0 else
                           int((now - started) * rate) - written)
            write_set = ([fd] if producing and written - received < window and
                         rate_budget > 0 else [])
            readable, writable, _ = select.select(
                read_set, write_set, [], 0.2)

            if fd in readable:
                chunk = os.read(fd, min(65536, written - received))
                expected = pattern_slice(received, len(chunk))
                if chunk != expected:
                    mismatch = next(
                        index for index, pair in enumerate(zip(chunk, expected))
                        if pair[0] != pair[1])
                    absolute = received + mismatch
                    raise RuntimeError(
                        f"{profile}: data mismatch at offset {absolute}: "
                        f"got 0x{chunk[mismatch]:02x}, "
                        f"expected 0x{expected[mismatch]:02x}")
                received += len(chunk)

            if fd in writable:
                size = min(16384, window - (written - received), rate_budget)
                try:
                    count = os.write(fd, pattern_slice(written, size))
                except BlockingIOError:
                    count = 0
                written += count

        elapsed = time.monotonic() - started
        return received, elapsed
    finally:
        os.close(fd)


def switch_profile(usb_device: Path, current_device: Path,
                   profile: str) -> Path:
    previous_devnum = read_text(usb_device / "devnum")
    subprocess.run(
        [SWITCH_PATH, str(current_device), "select", profile],
        stdout=subprocess.DEVNULL,
        check=True,
    )
    return tty_on_usb_device(
        usb_device, PROFILES[profile], previous_devnum)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("device", type=Path,
                        help="current emulator tty, e.g. /dev/ttyUSB1")
    parser.add_argument("profiles", nargs="*", choices=PROFILES,
                        default=list(PROFILES))
    parser.add_argument(
        "--stress-seconds", type=float, default=0.0,
        help="after boundary tests, stream and verify data for this many "
             "seconds per profile")
    parser.add_argument(
        "--stress-window", type=int, default=65536,
        help="maximum number of accepted but not yet echoed bytes")
    parser.add_argument(
        "--stress-rate", type=int, default=0,
        help="limit generated payload to bytes/s (zero means unlimited)")
    args = parser.parse_args()

    if args.stress_seconds < 0:
        parser.error("--stress-seconds must be non-negative")
    if args.stress_window <= 0:
        parser.error("--stress-window must be positive")
    if args.stress_rate < 0:
        parser.error("--stress-rate must be non-negative")

    usb_device = usb_device_for_tty(args.device)
    current_device = args.device
    for profile in args.profiles:
        current_device = switch_profile(usb_device, current_device, profile)
        verify_loopback(current_device, profile)
        if args.stress_seconds > 0:
            count, elapsed = stress_loopback(
                current_device, profile, args.stress_seconds,
                args.stress_window, args.stress_rate)
            rate = count / elapsed / (1024 * 1024)
            print(f"PASS {profile}: {current_device}, stress "
                  f"{count} bytes in {elapsed:.2f}s ({rate:.2f} MiB/s)",
                  flush=True)
        else:
            print(f"PASS {profile}: {current_device}", flush=True)

    # Exercise the management command implemented by the final profile too.
    switch_profile(usb_device, current_device, args.profiles[-1])
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError,
            TimeoutError) as error:
        print(f"verify-hardware: {error}", file=sys.stderr)
        raise SystemExit(1)
