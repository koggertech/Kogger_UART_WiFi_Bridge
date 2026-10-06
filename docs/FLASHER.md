# Wi‑Fi Module Flasher

A small Windows tool that writes the firmware into a module held in the ESP32‑C3 ROM bootloader: a new board, a board
with other firmware, or one that no longer starts. One window: a firmware file and a **Flash** button.

## How to use

1. Download `KoggerWiFiFlasher_EN.exe` and the firmware file `KoggerWiFi_X.Y.Z.ufww` from the
   [releases](https://github.com/koggertech/Kogger_UART_WiFi_Bridge/releases).
2. Connect a USB‑UART adapter (3.3 V levels) to connector **X1** of the module ([HARDWARE.md](HARDWARE.md)).
3. Hold **BOOT**, power the module, release BOOT.
4. Start `KoggerWiFiFlasher_EN.exe` and choose the `.ufww` file. The window shows
   "Wi‑Fi module firmware, version X.Y.Z"; any other file is refused and the button stays off.
5. Press **Flash**. When it says "Done: flashed and verified by hash", power the module off and on again, this time
   without BOOT.

## What it does

- Finds the serial port where an ESP32‑C3 bootloader answers; ports without one are left untouched.
- Writes, in one esptool call, the bootloader (0x0), the partition table (0x8000), the OTA data (0xF000) and the chosen
  firmware (0x20000); esptool checks the hash of every region. 460800 baud, retried at 115200.
- Leaves the settings area (NVS) alone: network, role, addresses and port rates are kept. A factory reset is BOOT held
  for 10 s on a running module.
- Checks the file the way the module does: ESP image, ESP32‑C3, application descriptor of this project, SHA‑256.
- The log of the last run: `%APPDATA%\KoggerWiFiFlasher\flash.log`.

A module that runs this firmware is updated without the bootloader, over Kogger SBP ([UPDATE.md](UPDATE.md)).

## From source

`tools/flasher/kogger_wifi_flasher.pyw` runs with Python 3, tkinter, esptool ≥ 5 and pyserial. The single
`.exe` is built with PyInstaller and checks itself (`--selftest`) right after the build:

```
python -m pip install esptool pyserial pyinstaller
python tools/flasher/make_exe.py --ufww KoggerWiFi_X.Y.Z.ufww    # -> dist/KoggerWiFiFlasher_EN.exe
```

The bootloader, partition table and OTA data are in `tools/flasher/parts/` with their hashes; they change only with
the bootloader or the partition layout (`python tools/flasher/make_parts.py --build firmware/build-uart`).
Windows Defender may distrust a fresh PyInstaller `.exe`; the `.pyw` does the same job.

Status: the tool's checks pass on a PC (file checks, parts, a run with no port and with no bootloader); flashing a
real module with it has not been verified yet.
