# gps_sniffer — listen-only UART identifier (Kogger Wi-Fi board)

A small ESP-IDF app for the ESP32-C3-MINI-1U Wi-Fi board. It finds the **pinout (which X2 pin is the
device's TX) and the baud rate** of an unknown serial line — made for tapping a spare connector of a boat's
GPS module that repeats the module's output, so the line into the autopilot is never touched. Separate
from `firmware/` (the shipping bridge); same board, same toolchain. After the first flash it is **updated
over its own X1 port, without the BOOT button** (`ota_push.py`).

## How it works

Listen-only: UART1 TX is parked on GPIO10 (not on any header) and never written, so no X2 pin is ever
driven. The app sweeps

```
RX pin ∈ {GPIO4 = X2 pin2, GPIO5 = X2 pin1}  ×  baud ∈ {9600,38400,115200,4800,19200,57600,230400,460800,921600}  ×  {normal, inverted}
```

listening ~1.5 s per combination (180 ms when the line is silent), and scores the bytes for a **valid
NMEA-0183 checksum** (`$…*HH`) or a **valid u-blox UBX checksum** (`B5 62 …`). The first valid lock wins;
the app then relays the line to X1 and repeats a `#LOCK` line every 3 s. With nothing locked it prints a
`#NOLOCK` line and sweeps again after 5 s. Trying RX on each X2 pin is the "software Rx/Tx swap": no
re-plugging.

## Wiring (power off)

| Device spare connector | → | Board |
|---|---|---|
| GND | → | X2 pin 3 (GND) — **connect first** |
| signal A | → | X2 pin 1 (GPIO5) |
| signal B | → | X2 pin 2 (GPIO4) |
| VCC | → | **leave unconnected** |

- The two signal lines go to X2 pin1 and pin2 in either order — the sweep finds which one is the TX.
- **Common GND is mandatory.** Do not connect the device VCC to the board.
- Power the board from the computer side: a 3.3 V USB-UART adapter on X1 (adapter TX → X1 pin2, RX →
  X1 pin1, GND → X1 pin3; it powers the board through X1 pin4).
- The RX inputs are 3.3 V CMOS (docs/HARDWARE.md: do not drive them from 5 V logic continuously).
  **Measure the idle level of the device's signal pins first**: a UART line idles at its logic high.

## X1 output (115200 8N1)

| Line | Meaning |
|---|---|
| `=== Kogger GPS sniffer <ver> (…, build <date time>) ===` | banner: which build is running |
| `#BOOT slot=ota_0\|ota_1 state=…` | running OTA slot; `pending` right after an update |
| `  RX GPIOn (X2.k)  <baud>  normal\|inverted  bytes=… print=…%  NMEA ok\|UBX ok\|silent  <sample>` | one sweep row |
| `#LOCK RX=GPIOn (X2.k) baud=… normal fmt=NMEA …` | the lock, repeated every 3 s between sentences |
| `#NOLOCK …` | nothing locked; a hint what to check |
| `$G…` | the relayed device line |
| `#OTA …` | update replies (below) |

LED (GPIO0): **on** = powered and running; **toggles** with relayed data; **toggles per chunk** during an update.

## Build

ESP-IDF 5.5. Keep the build directory outside a synced folder:

```
idf.py -C tools/gps_sniffer -B <build dir> -D SDKCONFIG=<build dir>/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults" reconfigure build
```

Delete `<build dir>/sdkconfig` when `sdkconfig.defaults` changes: the
defaults only fill missing keys. In PowerShell keep the quotes: an unquoted `-DSDKCONFIG_DEFAULTS=sdkconfig.defaults`
is split at the dot.

## First flash (once, with BOOT)

The first flash writes the bootloader and the two-slot partition table, so it goes through the ROM
loader. The board has no auto-reset: hold BOOT, re-apply power, release BOOT. Then, from `<build dir>`:

```
python -m esptool --chip esp32c3 -p <port> -b 460800 --before no_reset --after watchdog_reset write_flash \
  --flash_mode dio --flash_size 4MB --flash_freq 80m \
  0x0 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin \
  0xf000 ota_data_initial.bin 0x20000 gps_sniffer.bin
```

`--after watchdog_reset` restarts the chip into the app without a reset line (esptool 4.6+).

## Updates (no BOOT)

```
python tools/gps_sniffer/ota_push.py --port <port> <build dir>/gps_sniffer.bin
```

The running app takes the image over X1 into its other slot — chunks of 1 KB, each with a CRC-32 and an
acknowledgement, lost or damaged chunks repeated — checks the CRC-32 of the whole image and the IDF image
check, and reboots into it. The new image confirms itself after running 3 s; one that resets earlier is
rolled back by the bootloader to the previous slot. Exit code 0 means: written, booted (banner and `#BOOT`
from the new slot) and confirmed. A 250 KB image takes about 26 s at 115200. If an image ever breaks the
update path itself, the first-flash procedure above always recovers the board.

## Reading the result

- `NMEA ok` / `UBX ok` → that **RX pin is the device's TX wire**, at that **baud**; the other signal pin is
  the device's RX input — leave it.
- `silent` on every row of a pin → nothing on that wire (or it is the device's input).
- Bytes but no valid frame at any baud → likely a non-NMEA binary protocol or an unlisted baud: capture
  the raw bytes and decode them.
- No bytes anywhere → device powered? GND common? signal pins on X2 pin1/pin2?
