# Third‑party notices

## Included in this repository

**MAVLink message table.** `firmware/main/mavcrc.c` and `host/mavcrc_table.py` hold `CRC_EXTRA` values and payload lengths.
`tools/gen_mavcrc.py` generates them from `all/all.h` of the generated MAVLink C library
(<https://github.com/mavlink/c_library_v2>, branch master, fetched 2026‑09‑25). The MAVLink project states: "The message
definition XML files and the generated C‑language version of MAVLink (a header‑only library) are made available under
the MIT‑licence" (<https://mavlink.io/en/#license>). Its permission notice, from the exception in the MAVLink
generator's `COPYING`, is in [LICENSES/MAVLink-generated-MIT.txt](LICENSES/MAVLink-generated-MIT.txt).

## Used, not included

**ESP‑IDF** (Espressif Systems), Apache License 2.0, <https://github.com/espressif/esp-idf>. It is the build framework and
the libraries the firmware links against. Firmware binaries built from this repository contain ESP‑IDF (Apache‑2.0),
including Espressif's precompiled Wi‑Fi and PHY libraries (Apache‑2.0, `components/esp_wifi/lib/LICENSE`,
`components/esp_phy/lib/LICENSE`), and third‑party components that ESP‑IDF lists in its `COPYRIGHT.rst`: lwIP,
wpa_supplicant, FreeBSD net80211, newlib and the TLSF allocator (BSD), FreeRTOS (MIT), Mbed TLS (Apache‑2.0). Anyone who distributes
binaries must reproduce those notices
(<https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32c3/COPYRIGHT.html>).

**esptool** (Espressif Systems), GPL‑2.0‑or‑later, used as an external program by `tools/bench_flash.py`.

**pyserial**, BSD license, used by the bench tools. **matplotlib**, PSF‑based license, used by `tools/make_charts.py`.

## KiCad libraries

The schematic in `hardware/` embeds a few symbols of the KiCad standard libraries (power symbols, `PWR_FLAG`,
`TestPoint`), and the project library symbol `VCC_X2` is derived from KiCad `power:VCC`. The board refers to 3D models of the KiCad library by the `${KICAD10_3DMODEL_DIR}` path; they are not
included in this repository. The KiCad libraries are licensed CC‑BY‑SA 4.0 with an exception for designs that use
them (<https://www.kicad.org/libraries/license/>).

## Protocol compatibility

The firmware implements the Kogger SBP wire protocol and the firmware‑upgrade exchange of **KoggerApp**
(<https://github.com/koggertech/KoggerApp>, GPL‑3.0) for interoperability. No KoggerApp source code is included in this
repository. The KoggerApp file and function names in the documentation mark where the host side of the protocol lives.
