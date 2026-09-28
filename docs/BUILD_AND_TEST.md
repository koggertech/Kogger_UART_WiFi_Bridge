# Build, flash, test

## 0. PC tests (no hardware)

```
python tests/test_all.py
```

Expected: `43 checks, 0 failed`.
- The script compiles the firmware's portable C (`frame.c`, `proto.c`, `sbp.c`, `kframe.c`, `kpack.c`, `mavcrc.c`)
  with a host compiler.
- It compares the C output byte for byte with the Python mirrors in `host/`.
- Compiler: `$CC`, else gcc/cc/clang on PATH, else MSVC (found through vswhere). Without a compiler the C part fails;
  it is never skipped silently.

## 1. Build the firmware

ESP‑IDF v5.5.5 (tested); other 5.x versions may work but were not tested. In an ESP‑IDF shell:

```
cd firmware
# link to the host over UART0 (X1), 921600 baud
idf.py -B build-uart -D SDKCONFIG=build-uart/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.uart" build
# link to the host over the C3's native USB Serial/JTAG
idf.py -B build-usb -D SDKCONFIG=build-usb/sdkconfig build
```

- Keep one variant per build directory. `sdkconfig.defaults` only fills keys that are missing from an existing
  `sdkconfig`, so delete `build-*/sdkconfig` after editing a defaults file.
- A build‑time check refuses a configuration where the console and the host link share one port.
- The image is `build-uart/headunit_wifi_bridge.bin`, about 0.93 MB of a 1.875 MiB slot.
- The version comes from `PROJECT_VER` in `firmware/CMakeLists.txt`. The environment variable `WB_VERSION` overrides it
  for test images: the update tests need two different versions.

## 2. Flash

A board with auto‑reset wiring: `idf.py -B build-uart -p PORT flash`.

The reference board has no auto‑reset ([HARDWARE.md](HARDWARE.md)):
1. Hold BOOT, apply power, release BOOT.
2. Run:
   ```
   python tools/bench_flash.py --port COM3 --build-dir firmware/build-uart   # wait for the bootloader, flash, wait for a reset, smoke test
   ```
3. Remove and restore power without BOOT when the script asks.

`bench_flash.py` runs every esptool step in one stub session at 115200. Each esptool call without a reset leaves the
stub at its own rate, so the next call could not find it. The script takes the offsets from the build's fresh
`flash_args`: bootloader 0x0, partition table 0x8000, otadata 0xF000, application 0x20000. It needs esptool ≥ 5 and
pyserial.

Reading a full 4 MB flash in one esptool call failed twice with "Digest mismatch" through one CP2105 adapter; reading in
256 KB pieces worked. Data through such an adapter is occasionally corrupted. esptool catches it by MD5, and the
bridge's frames catch it by CRC‑16 (counter `link_crc` in `STATS`).

In the UART variant the firmware log goes to the native USB (if wired out). Apart from the ROM bootloader's message at
reset, which the host drops as junk, X1 carries only frames.

## 3. Bench checks (a PC, no host computer)

Flash and smoke test in one go, 11 checks, exit code 0 = all PASS:

```
python tools/bench_flash.py --port COM3 --build-dir firmware/build-uart
python tools/bench_flash.py --port COM3 --no-flash       # smoke test only
```

A board on factory settings (0.13) relays line 0, which locks X1 to SBP, so no `* HELLO` comes: the smoke test then
runs over SBP instead (discovery from address 87, link report, junk that must not restart the module).

By hand, over the text protocol:

```
python host/espwifi_ctl.py --serial COM3 ver
python host/espwifi_ctl.py --serial COM3 scan
python host/espwifi_ctl.py --serial COM3 connect "<SSID>" "<password>"
python host/espwifi_ctl.py --serial COM3 -e status
python host/espwifi_ctl.py --serial COM3 stats
```

Traffic through the IP bridge without a Linux host. The script plays the host 10.99.0.2 and sends ICMP as IP frames:

```
python tools/bench_ping.py --port COM3 --sweep 10.0.0.1-40           # who answers behind the access point
python tools/bench_ping.py --port COM3 -n 30 --size 1400 10.0.0.10    # loss, RTT, integrity
```

Criteria:
- `VER` answers `proto=1`, and `SCAN` shows the access point;
- a few seconds after `connect`, `status` answers `OK state=CONNECTED ... ip=...`;
- after a power cycle the module connects by itself (`STATUS` after ~5–20 s).

As a Kogger SBP device (what an SBP host sees), exit code 0 = all PASS:

```
python tools/bench_sbp.py --port COM3                          # identity, ID_WIFI, ID_WIFI_NET (reversible), errors
python tools/bench_sbp.py --port COM3 --role-test              # + reboot into the AP role and back
python tools/bench_sbp.py --port COM3 --bauds 115200,2000000   # + switch through rates and back
python tools/bench_sbp.py --port COM3 --write-test             # + a saved change of line 1, restored
```

- `--role-test` takes the module off its access point for the duration of the test.
- After every reboot the script looks for the module first at the address it had before (0.11 keeps it for one boot
  after a role change with reboot), then at its bridging address (88 after the switch to the access point) and finally at 0, 87
  and 88.
- `bench_sbp.py` scans and briefly changes settings, so do not run it on a module that carries live traffic.
- `tools/bench_ports.py --port COM3` checks the 0.12 port rules on the port it runs on (X1 or X2; `--baud` for another
  rate): discovery to 0 and 255, `ID_WIFI_NET` v7 pages, reports after a request, a rate change of this port
  (`ID_UART`) saved at once and kept 12 s without a request (0.15), `ID_WIFI_NET` v3 keeping this port's rate, the other port's
  rate saved at once, addresses 0 and 255 refused. It finds the module by discovery and puts every setting back,
  but a line that ran on its role's defaults is a saved record afterwards (it applies in both roles): on a module
  being set up for use, run it first or clear the lines with a role change that forgets them (`ID_WIFI_NET` v0 flag 2). `--long` also checks
  that the reports stop 60 s after the last request.

Relay, with the module and the PC on one IP network (the PC plays the peer over UDP and the host over serial):

```
python tools/bench_relay.py --port COM3 --pc-ip 192.168.1.10
```

Firmware update and its tests: [UPDATE.md](UPDATE.md) (`tools/sbp_update.py`; test images: §7 below). Full version, slot and rollback
state: `python tools/fwinfo.py --port COM3`. It does not work while line 0 bridges, because the port then speaks only
SBP.

## 4. Linux host with the IP bridge

The daemon uses only the Python standard library (termios); it was written for a head unit with a stock Python 3.12.

```
scp -r host user@<host>:/tmp/espwifi
ssh user@<host> "cd /tmp/espwifi && sudo sh install.sh"
```

- `install.sh` installs the daemon to `/opt/espwifi` and enables `espwifi.service`.
- A udev rule creates `/dev/espwifi` for the native USB variant.
- NetworkManager is told to leave `espwifi0` alone.
- For the UART variant, add `--port /dev/ttyUSB0` to `espwifi.service`, or enable the CP210x rule with the adapter's
  serial number in `99-espwifi.rules`.

Check on the host:

```
python3 /opt/espwifi/espwifi_ctl.py link          # state=UP
python3 /opt/espwifi/espwifi_ctl.py status
ip -br addr show espwifi0                          # 10.99.0.2 peer 10.99.0.1
ping -c3 10.99.0.1                                 # the module
ping -c3 -I espwifi0 <address behind the AP>       # through Wi-Fi
resolvectl status espwifi0                         # DNS 10.99.0.1
```

## 5. Hardware outputs

```
python tools/make_hw_outputs.py   # hardware/*.kicad_* -> hardware/outputs (PDF, fab zip, BOM, P&P, ERC/DRC), docs/img renders
```

Needs `kicad-cli` of KiCad 10 (`--kicad-cli PATH` or `KICAD_CLI`). The fab zip holds the Gerbers (4 copper
layers, solder mask, paste and silkscreen of both sides, board outline; Protel extensions), the Excellon drill file, a drill map, the Gerber job file, the BOM and the pick‑and‑place.

## 6. Documentation assets

The charts in `docs/img/` and the datasheet `docs/KoggerWiFi_datasheet.pdf` are generated:

```
python tools/make_charts.py        # docs/img/*.svg (matplotlib); the relay chart runs host/kframe.py
python tools/make_datasheet.py     # docs/datasheet/datasheet.html -> docs/KoggerWiFi_datasheet.pdf (Edge or Chrome, headless)
```

`make_charts.py` output is deterministic: the same sources and matplotlib version give byte‑identical SVG files.

## 7. Test images for the update tests

The update tests need images with another version, and the rollback test an image that never confirms. Build each in
its own directory (`sdkconfig.defaults` only fills keys missing from an existing `sdkconfig`), in an ESP‑IDF shell:

```
cd firmware
# PowerShell: $env:WB_VERSION = "0.99.0"      (bash: export WB_VERSION=0.99.0)
idf.py -B build-v099 -D SDKCONFIG=build-v099/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.uart" build
# PowerShell: $env:WB_VERSION = "0.98.0"
idf.py -B build-noconf -D SDKCONFIG=build-noconf/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.uart;sdkconfig.defaults.test_noconfirm" build
cd ..
python tools/make_update.py --build firmware/build-v099 --out dist/test     # update, --drop, --corrupt, --stop
python tools/make_update.py --build firmware/build-noconf --out dist/test   # --expect-rollback
```

The second number of the version must differ from the running firmware's (`ID_VERSION` carries only major.minor).

