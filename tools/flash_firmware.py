#!/usr/bin/env python3
"""Update a connected ParaLink while preserving settings and user data."""

import argparse
import json
import os
import re
import struct
import subprocess
import sys
import time
import zlib
from hashlib import sha256
from pathlib import Path

import serial
from serial.tools.list_ports import comports

ROOT = Path(__file__).resolve().parents[1]
MODELS = {"h717": "H717", "k191": "K191"}
RUNTIME_ID = (0x303A, 0x0002)
BOOT_ID = (0x303A, 0x1001)


def matches_usb(port, identity):
    return (port.vid, port.pid) == identity


def serial_key(port):
    return (port.serial_number or "").replace(":", "").upper()


def wait_for_port(identity, device_serial, description=None):
    until = time.monotonic() + 15
    while time.monotonic() < until:
        for port in comports():
            if (matches_usb(port, identity) and serial_key(port) == device_serial
                    and (description is None or port.description == description)):
                return port
        time.sleep(0.2)
    return None


def legacy_board(port):
    # Only used to migrate devices installed before MAC-based USB names.
    return {"ParaLab Link": "h717", "ParaLink A191": "h717", "ParaLink K191": "k191"}.get(port.description)


def product_name(port):
    return f"ParaLink {serial_key(port)[-4:]}"


def runtime_port(port):
    return matches_usb(port, RUNTIME_ID) and (port.description == product_name(port) or legacy_board(port))


def verified_image(dist, manifest, name, suffix, address):
    image = dist / f"paralink-{manifest['board']}-{manifest['version']}-{suffix}.bin"
    component = next(part for part in manifest["components"] if part["name"] == name)
    data = image.read_bytes()
    if (component["offset"] != address or len(data) != component["bytes"]
            or sha256(data).hexdigest() != component["sha256"]):
        raise ValueError(f"{image.name} does not match the release manifest")
    return image


def release_for(board):
    dist = ROOT / "dist"
    manifest = json.loads((dist / f"manifest-{board}.json").read_text())
    if manifest["board"] != board:
        raise ValueError("Release manifest does not match the selected hardware")
    app = verified_image(dist, manifest, "firmware.bin", "app", "0x20000")
    assets = verified_image(dist, manifest, "assets.bin", "assets", "0x320000")
    return manifest, app, assets


def ota_frame(offset, chunk):
    payload = struct.pack("<II",offset,zlib.crc32(chunk)) + chunk
    return b"\xc0" + payload.replace(b"\xdb",b"\xdb\xdd").replace(b"\xc0",b"\xdb\xdc") + b"\xc0"


def ota(port, board):
    with serial.Serial(port.device, baudrate=460800, timeout=20, write_timeout=10, exclusive=True) as usb:
        time.sleep(0.12)
        usb.baudrate = 2400
        usb.timeout = 4
        hello = usb.readline().decode("ascii", "replace").strip().split()
        if len(hello) != 4 or hello[0] != "PARALINK" or hello[1] not in ("OTA","OTA2"):
            raise ValueError("No OTA response. Finish any transfer and retry, or use ROM mode for recovery.")
        usb.timeout = 20
        detected = hello[2].lower()
        if detected not in MODELS or (board and detected != board):
            raise ValueError(f"Selected firmware does not match the {hello[2]} device.")
        manifest, app, assets = release_for(detected)
        data, asset_data = app.read_bytes(), assets.read_bytes()
        asset_crc = struct.unpack_from("<I", asset_data, 12)[0]
        version = manifest["version"]

        def expect(value):
            response = usb.readline().strip()
            if response != value:
                detail = response.decode("ascii", "replace") if response else "no response"
                raise ValueError(f"USB update interrupted ({detail}). Check the device display.")

        print(f"{product_name(port)} · {hello[2]} · v{hello[3]} -> v{version}", flush=True)
        usb.write(f"UPDATE {hello[2]} {version} {len(data)} {len(asset_data)} {asset_crc:08x} {sha256(data).hexdigest()}\n".encode())
        framed = hello[1] == "OTA2"
        expect(b"OK 0" if framed else b"OK")
        if framed:
            usb.timeout=1.2
        last_progress = -1
        verified = False
        for offset in range(0, len(data), 1024):
            chunk = data[offset:offset + 1024]
            end = offset + len(chunk)
            if framed:
                wire = ota_frame(offset, chunk)
                wanted = f"OK {end}".encode()
                for attempt in range(7):
                    usb.write(wire)
                    deadline = time.monotonic() + 1.2
                    while time.monotonic() < deadline:
                        response = usb.readline().strip()
                        if response == b"DONE" and end == len(data):
                            verified = True
                            break
                        if response == wanted or response.startswith(b"ERROR"):
                            break
                    if response == wanted or verified:
                        break
                    if response.startswith(b"ERROR"):
                        raise ValueError(response.decode("ascii", "replace"))
                    print(f"\nRetrying firmware block at {offset}", flush=True)
                else:
                    raise ValueError(f"No valid ACK for firmware block at {offset}")
            else:
                usb.write(chunk)
                expect(b"OK")
            progress = min(99, (offset + len(chunk)) * 100 // len(data))
            if progress != last_progress:
                print(f"\rWriting firmware {progress:3d}%", end="", flush=True)
                last_progress = progress
        print("\rVerifying firmware     ", flush=True)
        if not verified:
            usb.timeout=15
            expect(b"DONE")
    print("Verified 100%. Restarting.", flush=True)
    time.sleep(3)
    return manifest


def flash(pio_core, args):
    ports = comports()
    board = args.board
    if args.boot_port:
        if not board:
            raise ValueError("Select --board with --boot-port")
        candidates = [p for p in ports if p.device == args.boot_port and matches_usb(p, BOOT_ID)]
    else:
        candidates = [p for p in ports if runtime_port(p)
                      and (not board or not legacy_board(p) or legacy_board(p) == board)
                      and (not args.port or p.device == args.port)]
        matching_legacy = [p for p in candidates if board and legacy_board(p) == board]
        if len(matching_legacy) == 1 and not args.port:
            candidates = matching_legacy
    if len(candidates) != 1 or not serial_key(candidates[0]):
        raise ValueError("Select one connected ParaLink with --port. For ROM use --board and --boot-port.")

    port = candidates[0]
    board = board or legacy_board(port)
    device_serial = serial_key(port)
    description = product_name(port)
    if not args.boot_port and not (args.rom or args.with_assets) and port.description not in ("ParaLab Link", "ParaLink A191"):
        manifest = ota(port, board)
        return wait_for_port(RUNTIME_ID, device_serial, description), manifest

    if not board:
        raise ValueError("Specify --board when updating through ROM mode")
    manifest, app, assets = release_for(board)
    print(f"{description} · {MODELS[board]} · target v{manifest['version']}", flush=True)
    if not args.boot_port:
        with serial.Serial(port.device, baudrate=1200, exclusive=True):
            time.sleep(0.18)
        port = wait_for_port(BOOT_ID, device_serial)
        if port is None:
            raise ValueError("Device did not enter ROM download mode. Close serial connections, or use BOOT + RESET.")

    esptool = (pio_core / "packages/tool-esptoolpy/esptool.py").resolve(strict=True)
    base = [sys.executable, str(esptool), "--chip", "esp32s3", "--port", port.device, "--before", "no_reset"]
    check_flash = ["--after", "no_reset", "--no-stub", "flash_id"]
    try:
        probe = subprocess.check_output(base + check_flash, text=True)
    except subprocess.CalledProcessError:
        if not args.boot_port:
            raise
        probe = subprocess.check_output(base[:-1] + ["usb_reset"] + check_flash, text=True)
    size = re.search(r"Detected flash size: (\d+)MB", probe)
    if not size or int(size[1]) * 1024 * 1024 != manifest["flash_bytes"]:
        raise ValueError("Detected flash size does not match the selected board. No image was written.")
    # Either app slot may be active after a previous OTA installation.
    images = ["0x20000", str(app), "0x1a0000", str(app)]
    if args.with_assets:
        images.extend(["0x320000", str(assets)])
    subprocess.run(base + ["--baud", "460800", "--after", "watchdog_reset", "write_flash", *images], check=True)
    return wait_for_port(RUNTIME_ID, device_serial, description), manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--boot-port", help="Use this port in ROM download mode")
    source.add_argument("--port", help="Select a running ParaLink by its serial port")
    parser.add_argument("--board", choices=MODELS)
    parser.add_argument("--rom", action="store_true", help="Enter ROM recovery mode from the selected USB port")
    parser.add_argument("--with-assets", action="store_true", help="Use ROM to update the firmware and catalog assets")
    parser.add_argument("--pio-core", type=Path, default=os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
    args = parser.parse_args()
    try:
        port, manifest = flash(args.pio_core.expanduser(), args)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")
    if port is None:
        parser.exit(1, "Write verified, but ParaLink did not return to its USB port\n")
    print(f"{port.description} v{manifest['version']} is ready on {port.device}")


if __name__ == "__main__":
    main()
