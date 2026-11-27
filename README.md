# ParaLink

ParaLink is a handheld companion and USB–UART bridge for **Tamagotchi Paradise**. Use a touchscreen or two buttons to browse and send items, look up Lab Codes, and start Playdates. Connect the board to a computer to use it with tools that support USB serial.

**Version 0.1.18** · [Quick start](#quick-start) · [Features](#features) · [Wiring](#wiring) · [Build and flash](#build-and-flash) · [Source layout](#source-layout) · [Credits and licensing](#credits-and-licensing)

## Quick start

| Build | Board | Controls |
| --- | --- | --- |
| **H717** | LILYGO T-Display-S3 AMOLED 1.91-inch touch, 16 MiB flash | Touch and BOOT |
| **K191** | LILYGO T-QT Pro N8, 8 MiB flash, 128 × 128 LCD | A (GPIO0) and B (GPIO47) |

Select the matching build before the first installation. The USB name is **ParaLink** followed by the last four hexadecimal digits of the board’s factory MAC. The same suffix appears next to the version in the title bar.

When you plug in ParaLink, Chrome can show a USB notification. Click it to open [ParaLab’s Device page](https://x43x61x69.github.io/ParaLab/#device), then use the page’s USB connection controls.

## Features

| Feature | How to use it |
| --- | --- |
| **Gifts** | Choose a gift and tap **Send Gift** on ParaLink. On Paradise, open **Connect → Gift** and press **B**. The receiving ROM must support the item. The gift details show its Shop Code when available. |
| **Lab Items · Coins** | Send any of the 67 bundled Lab Items or a Money Boost. First open **Connect → Download** on Paradise. |
| **Playdates** | Choose a character by world, including 86 visitors from **Neon Planets**. Start a Playdate on Paradise and follow the prompts on both devices. |
| **Lab Codes** | Look up 39 Lab Sticker Codes in **Lab Codes**. Enter them under **Lab Log → Microscope → Lab Code** on Paradise. |
| **USB Bridge · Link Test** | View USB and UART traffic, replay the most recently captured segment, or test the UART connection. |
| **Favorites · Last Item** | Open your favorites or the last item sent successfully. Favorites are stored in nonvolatile storage. The last-item entry clears on restart. |
| **Settings** | Adjust brightness, rotate the screen and touch orientation, choose an item-name language, or view device information. |

The title bar shows your page number on screens with multiple pages. Tap the right arrow or swipe to change pages. Hold the up arrow for about 0.65 seconds to return to the previous screen. The progress line lights up while you hold it, including on the first page and on single-page screens. Item lists show four entries per page. Select an item to add it to favorites or tap **Continue** for transfer instructions. Tap **Cancel** to leave the confirmation screen or **Stop** to interrupt a transfer. A short press of the physical **BOOT** button goes back. A long press returns home. Pressing it during a transfer requests a stop.

**More** opens favorites, the last item, settings, the USB bridge, the connection test, and the guide. The first page of the guide shows the wiring.

The screen turns off after 30 seconds without input or USB or UART traffic. The bridge keeps running. Touch the screen, press **BOOT**, or send data to wake the display. The first touch used to wake the display only turns it on.

A local transfer pauses the USB bridge while it owns the UART. The bridge resumes after the operation finishes. Start one transfer at a time from either the computer or the touchscreen.

### K191 controls

| Input | Action |
| --- | --- |
| A short | Move the selection. Cancel a confirmation or stop an active transfer. |
| A hold | Go back one screen. |
| B short | Select, open, or confirm the action. |
| B hold | Scroll by pages in a list, or add or remove a favorite on an item’s detail screen. |

The K191 display is rotated 90° clockwise and shows four rows per page. Either button wakes the display. The first press only wakes it. A transfer starts only after the confirmation screen.

### Item-name language

Item names default to **English**. Choose a language in **More → Settings → Item names**. ParaLink saves the setting and applies it to lists, details, and transfer confirmations. Available languages are English, French, Spanish, German, Portuguese, Italian, Japanese, Simplified Chinese, Korean, and Traditional Chinese. This setting changes the names shown on screen. [Official Paradise language list](https://tamagotchi-official.com/us/series/paradise/faq/)

## Wiring

Use a USB-C data cable, a plug that matches the board’s four-pin socket, and a three-contact Paradise UART adapter. Use the JH socket on K191 and the labeled UART socket on H717. Check the pin labels or continuity before connecting the cable. [K191 pinout](https://raw.githubusercontent.com/Xinyuan-LilyGO/T-QT/main/image/pinmap_en.jpg)

Face the back of the H717 with USB-C at the bottom. Use the four-hole **UART** socket to the right of USB-C and count from left to right. The cable plug is mirrored when viewed from its mating face. Identify each lead by its pin label or continuity. [LILYGO pinout photo](https://cdn.shopify.com/s/files/1/0617/7190/7253/files/T-Display-S3-AMOLED-Touch_1.jpg?v=1736151639)

Open the Paradise top cover and hold the device with its screen facing you and its buttons below the screen. Count the three contacts from left to right. [Paradise contacts photo](https://github.com/GMMan/tama-para-research/blob/master/hardware/prongs.jpg)

| UART signal | Tamagotchi Paradise contact |
| --- | --- |
| 1 · GND | 2 · Center · GND |
| 2 · 3V3 | Leave disconnected and insulate the lead |
| 3 · TX (GPIO43) | 3 · Right · RX |
| 4 · RX (GPIO44) | 1 · Left · TX |

Power the two devices separately. GPIO43 can carry ESP32 boot messages during startup. Finish flashing or restarting ParaLink before connecting the Paradise signal wires.

## Build and flash

From the project root, install PlatformIO and package the build using the same Python environment.

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install platformio
pio run -d firmware
python tools/package_firmware.py
```

PlatformIO uses `espressif32@6.12.0`. The build writes the images and SHA-256 digests to the local `dist/` directory. The firmware version comes from [link_version.h](firmware/include/link_version.h).

To enter download mode, hold the board with the screen facing you and the USB-C port pointing down. Hold **BOOT** on the left, tap **RESET** on the right, then release **BOOT**. Set `LINK_PORT` to the serial port that appears. Use esptool 4.9.0 for the commands below.

```sh
python -m pip install 'esptool==4.9.0'
```

### First installation

The full image contains the bootloader, partition table, firmware, asset pack, and an empty LittleFS filesystem. Flashing it overwrites the device’s firmware and stored data.

```sh
python -m esptool --chip esp32s3 --port "$LINK_PORT" --baud 460800 \
  --after watchdog_reset write_flash 0x0 dist/paralink-h717-0.1.18-factory.bin
```

### Update an installed device

Use **USB OTA** in the ParaLink browser updater or run the command below. The device and computer show the installed version, target version and progress. ParaLink writes the inactive app slot, verifies the firmware and switches boot slots only after validation.

```sh
python tools/flash_firmware.py
```

With both boards connected, pass `--port` with the port whose USB name matches the MAC suffix on the screen. Use the **ROM update** option when the asset pack changes. This preserves settings and favorites with a matching partition table. For a legacy K191 using the former GPIO17/18 wiring, move the signals to the JH socket and use the ROM update to install the new asset pack:

```sh
python tools/flash_firmware.py --board k191 --boot-port "$LINK_PORT" --with-assets
```

For a first installation on K191, use `dist/paralink-k191-0.1.18-factory.bin` in the esptool command above. The ROM loader shows progress on the computer.

## Source layout

| Path | Contents |
| --- | --- |
| [firmware/src](firmware/src/) | Touch interface, display drivers, UART protocol, and USB bridge |
| [firmware/include](firmware/include/) | Board pins, product name, version, and shared interfaces |
| [firmware/generated](firmware/generated/) | Built-in catalog, glyph tables, and color palette |
| [asset-pack](asset-pack/) | Asset image and catalog data |
| [tools](tools/) | Firmware packaging and glyph generation tools |
| [tests](tests/) | Button, bridge arbitration, and packet codec tests |

<details>
<summary>Regenerate the glyph tables</summary>

```sh
python -m pip install -r tools/requirements.txt
python tools/build_catalog.py
python tools/build_item_names.py
python tools/build_fonts.py /path/to/fonts
```

The font directory must contain `AtkinsonHyperlegible-Regular.ttf`, `AtkinsonHyperlegible-Bold.ttf`, `NotoSansCJKtc-Medium.otf`, and `NotoEmoji-Regular.ttf`. To reimport item names, first run `node tools/import_item_names.cjs /path/to/ParaLab /path/to/rom-dumps`. Standard builds can use the included item names and glyph tables.

To sync Lab Sticker and Shop Codes, run `node tools/import_codes.cjs /path/to/ParaLab`, then regenerate the catalog and item name headers.

To import new Lab Item IDs, first run `node tools/import_lab_items.cjs /path/to/ParaLab`. To rebuild the visitor assets from your own Neon Planets ROM, run `node tools/import_neon_visitors.cjs /path/to/ParaLab /path/to/neon-rom.bin`. Then reimport the item names and run the three generation commands above. Install the rebuilt asset pack with `--with-assets`.

</details>

## Credits and licensing

| Component | Source |
| --- | --- |
| USB–UART bridge foundation | [TamaHome-ESP32-C3-UART-USB](https://github.com/FattLlama/TamaHome-ESP32-C3-UART-USB) |
| Multilingual item names | ParaLab’s gift catalog, Lab Item ARC2 names, and Paradise ROM text tables |
| Latin letters and numerals | Atkinson Hyperlegible Regular and Bold, [OFL license](licenses/Atkinson-Hyperlegible-OFL.txt) |
| Chinese, Japanese, and Korean text | Noto Sans CJK TC Medium, [OFL license](licenses/Noto-CJK-OFL.txt) |
| Emoji in Japanese item names | Noto Emoji, [OFL license](licenses/Noto-Emoji-OFL.txt) |

The firmware is licensed under [GPL-3.0-or-later](LICENSE). ParaLink is an unofficial fan project. Third-party trademarks, characters, and assets belong to their respective rights holders.
