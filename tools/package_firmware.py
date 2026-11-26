#!/usr/bin/env python3
"""Package the standalone build and verify its flash layout."""

import argparse
import csv
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path
from tempfile import TemporaryDirectory


ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware"
FLASH_BYTES = 16 * 1024 * 1024
PARTITION_ENTRY = struct.Struct("<HBBII16sI")


def read_version():
    header = (FIRMWARE / "include/link_version.h").read_text()
    match = re.search(r'^#define PARALAB_LINK_VERSION "(\d+\.\d+\.\d+)"$', header, re.MULTILINE)
    if not match:
        raise ValueError("Set PARALAB_LINK_VERSION in firmware/include/link_version.h")
    return match.group(1)


def read_partitions(compiled_table):
    with (FIRMWARE / "partitions.csv").open(newline="") as file:
        rows = csv.reader(line for line in file if not line.startswith("#"))
        expected = {row[0].strip(): (int(row[3], 0), int(row[4], 0)) for row in rows if row}

    compiled = {}
    for magic, _, _, offset, size, label, _ in PARTITION_ENTRY.iter_unpack(compiled_table.read_bytes()):
        if magic != 0x50AA:
            break
        compiled[label.rstrip(b"\0").decode("ascii")] = (offset, size)

    if compiled != expected:
        raise ValueError("Built partition table differs from firmware/partitions.csv")
    return expected


def digest(data):
    return hashlib.sha256(data).hexdigest()


def package(pio_core):
    version = read_version()
    build = FIRMWARE / ".pio/build/standalone"
    packages = pio_core / "packages"
    dist = ROOT / "dist"
    dist.mkdir(exist_ok=True)
    stem = f"paralab-link-{version}"
    partitions = read_partitions(build / "partitions.bin")

    with TemporaryDirectory(dir=dist) as staging:
        staging = Path(staging)
        empty = staging / "empty"
        empty.mkdir()
        userdata = staging / "userdata.bin"
        subprocess.run(
            [str(packages / "tool-mklittlefs/mklittlefs"), "-c", str(empty),
             "-s", str(partitions["userdata"][1]), "-b", "4096", "-p", "256", str(userdata)],
            check=True,
        )

        sources = (
            ("bootloader.bin", build / "bootloader.bin", 0, 0x8000),
            ("partitions.bin", build / "partitions.bin", 0x8000, 0x1000),
            ("boot_app0.bin", packages / "framework-arduinoespressif32/tools/partitions/boot_app0.bin", *partitions["otadata"]),
            ("firmware.bin", build / "firmware.bin", *partitions["app0"]),
            ("assets.bin", ROOT / "asset-pack/assets.bin", *partitions["assets"]),
            ("userdata.bin", userdata, *partitions["userdata"]),
        )
        factory = staging / f"{stem}-factory.bin"
        # DIO is the ROM bootstrap mode. The second-stage loader enables QIO.
        command = [
            sys.executable, str(packages / "tool-esptoolpy/esptool.py"), "--chip", "esp32s3",
            "merge_bin", "--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", "16MB",
            "-o", str(factory),
        ]
        components = []
        for name, path, offset, limit in sources:
            data = path.read_bytes()
            if not 0 < len(data) <= limit or offset + limit > FLASH_BYTES:
                raise ValueError(f"{name} does not fit the flash layout")
            command.extend([hex(offset), str(path)])
            components.append({"name": name, "offset": hex(offset), "bytes": len(data), "sha256": digest(data)})

        subprocess.run(command, check=True)
        image = factory.read_bytes()
        if len(image) != FLASH_BYTES:
            raise ValueError(f"Expected {FLASH_BYTES} factory bytes, got {len(image)}")
        for part in components[1:]:
            # esptool may update the bootloader flash parameters and its hash.
            offset = int(part["offset"], 16)
            if digest(image[offset:offset + part["bytes"]]) != part["sha256"]:
                raise ValueError(f"Factory image does not match {part['name']}")

        manifest = {
            "version": version,
            "chip": "ESP32-S3R8",
            "flash_bytes": FLASH_BYTES,
            "factory_image": factory.name,
            "image_bytes": len(image),
            "sha256": digest(image),
            "components": components,
        }
        manifest_path = staging / "manifest.json"
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
        app = staging / f"{stem}-app.bin"
        assets = staging / f"{stem}-assets.bin"
        shutil.copyfile(build / "firmware.bin", app)
        shutil.copyfile(ROOT / "asset-pack/assets.bin", assets)
        for path in (factory, app, assets, manifest_path):
            path.replace(dist / path.name)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pio-core", type=Path, default=os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
    args = parser.parse_args()
    manifest = package(args.pio_core.expanduser())
    print(f"{manifest['factory_image']}  {manifest['sha256']}")


if __name__ == "__main__":
    main()
