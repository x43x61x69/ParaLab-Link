#!/usr/bin/env python3
"""Update the connected ParaLab Link while preserving its settings and user data."""

import argparse
import json
import os
import subprocess
import sys
import time
from hashlib import sha256
from pathlib import Path

import serial
from serial.tools.list_ports import comports


ROOT = Path(__file__).resolve().parents[1]
PRODUCT = "ParaLab Link"
RUNTIME_ID = (0x303A, 0x0002)
BOOT_ID = (0x303A, 0x1001)


def matches_usb(port, identity):
    return (port.vid, port.pid) == identity


def serial_key(port):
    return (port.serial_number or "").replace(":", "").upper()


def wait_for_port(identity, device_serial, description=None):
    until = time.monotonic() + 12
    while time.monotonic() < until:
        for port in comports():
            if (matches_usb(port, identity) and serial_key(port) == device_serial
                    and (description is None or port.description == description)):
                return port
        time.sleep(0.2)
    return None


def verified_image(dist, manifest, name, suffix, address):
    image = dist / f"paralab-link-{manifest['version']}-{suffix}.bin"
    component = next(part for part in manifest["components"] if part["name"] == name)
    data = image.read_bytes()
    if (component["offset"] != address or len(data) != component["bytes"]
            or sha256(data).hexdigest() != component["sha256"]):
        raise ValueError(f"{image.name} does not match the release manifest")
    return image


def flash(pio_core, boot_path, with_assets):
    dist = ROOT / "dist"
    manifest = json.loads((dist / "manifest.json").read_text())
    app = verified_image(dist, manifest, "firmware.bin", "app", "0x20000")
    images = ["0x20000", str(app)]
    if with_assets:
        assets = verified_image(dist, manifest, "assets.bin", "assets", "0x320000")
        images.extend(["0x320000", str(assets)])
    esptool = (pio_core / "packages/tool-esptoolpy/esptool.py").resolve(strict=True)
    ports = comports()
    if boot_path:
        candidates = [port for port in ports if port.device == boot_path and matches_usb(port, BOOT_ID)]
    else:
        candidates = [port for port in ports if matches_usb(port, RUNTIME_ID) and port.description == PRODUCT]
    if len(candidates) != 1 or not serial_key(candidates[0]):
        raise ValueError("Connect exactly one ParaLab Link, or specify its ROM port with --boot-port")

    port = candidates[0]
    device_serial = serial_key(port)
    if not boot_path:
        with serial.Serial(port.device, baudrate=1200, exclusive=True):
            time.sleep(0.15)
        port = wait_for_port(BOOT_ID, device_serial)
        if port is None:
            raise ValueError("The device did not enter download mode. Close the bridge connection, or use BOOT + RESET and --boot-port")

    subprocess.run(
        [sys.executable, str(esptool), "--chip", "esp32s3", "--port", port.device, "--baud", "460800",
         "--before", "no_reset", "--after", "watchdog_reset", "write_flash", *images],
        check=True,
    )
    return wait_for_port(RUNTIME_ID, device_serial, PRODUCT)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--boot-port", help="Use this serial port if the device is already in ROM download mode")
    parser.add_argument("--with-assets", action="store_true", help="Update the matching catalog assets as well")
    parser.add_argument("--pio-core", type=Path, default=os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
    args = parser.parse_args()
    try:
        port = flash(args.pio_core.expanduser(), args.boot_port, args.with_assets)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")
    if port is None:
        parser.exit(1, "Flash write verified, but ParaLab Link did not return to its USB port\n")
    print(f"{PRODUCT} is ready on {port.device}")


if __name__ == "__main__":
    main()
