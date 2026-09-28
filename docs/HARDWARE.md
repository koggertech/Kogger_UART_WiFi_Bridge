# Hardware

The reference board: an **ESP32‑C3‑MINI‑1U** module (ESP32‑C3, 4 MB flash, U.FL connector for an external 2.4 GHz
antenna) on a 20 × 30 mm 4‑layer board. The KiCad project, schematic PDF, Gerbers, BOM and pick‑and‑place are in
[`../hardware/`](../hardware/). The circuit and the checks made on the design files are described in
[`../hardware/README.md`](../hardware/README.md).

![Board](img/board_iso.png)

## Connectors

JST GH 4‑pin headers, 1.25 mm, right angle (SM04B‑GHS‑TB); the mating housing is GHR‑04V‑S.

| Connector | Pin | Function | ESP32‑C3 |
|---|---|---|---|
| X1 | 1 | TX of the module, 150 Ω series | GPIO21 (U0TXD) |
| X1 | 2 | RX of the module, 1 kΩ series | GPIO20 (U0RXD) |
| X1 | 3 | GND | — |
| X1 | 4 | VCC input, through the reverse‑polarity Schottky diode D9 | — |
| X2 | 1 | TX of the module, 150 Ω series | GPIO5 |
| X2 | 2 | RX of the module, 1 kΩ series | GPIO4 |
| X2 | 3 | GND | — |
| X2 | 4 | VCC_X2: VCC through R8 (0 Ω, fitted), or about 3 V from +3.3 V through R9 + D8 when R8 is left out | — |

- Signal levels are 3.3 V CMOS. Schottky clamps protect every line against over‑voltage, but do not drive the
  RX pins from 5 V logic continuously.
- **X1 = line 0 = UART0.** This is the host port and also the ROM bootloader's port, so X1 is the flashing port.
- **X2 = line 1 = UART1** on GPIO5 (TX) / GPIO4 (RX). On this board X2 is wired to these two pins only; the firmware
  can move line 1 to {0, 1, 3, 4, 5, 6, 7, 10} for other boards (GPIO0 drives the LED here, GPIO1/3/6/7/10 are not
  brought out), effective after a reboot.
- The firmware enables a weak pull‑up on both RX pins, so an unconnected X2 does not feed noise into the network.

## Power

| | |
|---|---|
| Input | X1 pin 4 (VCC) through D9, SDM4A40EP3 Schottky |
| Regulator | TI LMZM23601V3 step‑down module: 4–36 V in, 3.3 V out, up to 1 A (TI data) |
| VCC range | 4.5–24 V, rated (not measured on a board): the regulator's minimum plus the diode drop, and the 25 V input capacitors (C5/C6 22 µF 25 V) |
| Consumption | not measured; plan for the Wi‑Fi transmit peaks of the ESP32‑C3‑MINI‑1U (Espressif datasheet) |

Through R8, X2 pin 4 carries VCC to a device on X2. The board can also be powered from X2 pin 4, but that bypasses
the reverse‑polarity diode.

## Other parts

| Part | Connection | Use |
|---|---|---|
| LED VD1 (green) | GPIO0 → 1 kΩ → LED → GND, lit when high | blinks while a reset with the BOOT button is armed (below), otherwise off |
| Button SB1 | GPIO9 to GND, 10 kΩ pull‑up | held at power‑up: ROM download mode; held 5 s / 10 s while the firmware runs: resets (below) |
| Pull‑ups 10 kΩ | GPIO2, GPIO8 | strapping pins for a normal boot |
| EN | 10 kΩ pull‑up, 1 µF to GND | power‑on reset delay |
| Test pads, bottom | TP1 DM (GPIO18), TP2 DP (GPIO19), TP3 TX / TP4 RX of line 1 | USB Serial/JTAG flashing, line 1 probing |

There is no reset button and no auto‑reset circuit (DTR/RTS). The module resets when its supply is removed for 1–2 s.

## Resets with the BOOT button (0.13)

With the firmware running, hold BOOT (SB1), then release it:

| Held | LED | On release |
|---|---|---|
| under 5 s | — | nothing |
| 5 … 10 s | blinks slowly, 250 ms on / 250 ms off | the saved port rates are erased: X1 and X2 at 921600; nothing else changes |
| 10 s or more | blinks fast, 100 ms on / 100 ms off | every setting is erased: the factory settings below |

Either way the module restarts and resets before it reads any setting. Factory settings: Wi‑Fi station whose saved network is the factory network `KoggerBridge`, password `KoggerBridge` (build options
`WB_FACTORY_SSID` / `WB_FACTORY_PASS`); in the access point role the module's own network has the same name and
password. Line 0 (X1) relays to UDP 10.0.0.10:14444 and line 1 (X2) to 10.0.0.10:14445, the access point's default
address; both ports at 921600 baud; own address 87. A module switched to the access point role with factory settings
listens on the same ports, so two factory modules link X1 to X1 and X2 to X2.

- The 5 s press is the way back from a port rate a host cannot follow: every rate change is saved at once (0.15).
- A press shorter than 5 s does nothing, and neither does a button that is already down when the firmware starts.
- Until a freshly updated image has confirmed itself ([UPDATE.md](UPDATE.md)) the button does nothing: a restart then
  would roll the update back.
- If line 1 has been moved onto GPIO0 the LED stays dark; the resets still work.
- Held while power is applied, BOOT selects the ROM download mode instead (next section).

## Flashing without auto‑reset

1. Connect a 3.3 V USB‑UART adapter to X1: adapter TX → X1 pin 2, adapter RX → X1 pin 1, GND → pin 3. Power the
   board through X1 pin 4 or from a bench supply.
2. Hold BOOT (SB1), apply power, release BOOT. The chip is now in the ROM bootloader.
3. Flash with `tools/bench_flash.py`, which runs every esptool step in one session
   ([BUILD_AND_TEST.md](BUILD_AND_TEST.md) §2).
4. Remove and restore power without BOOT to start the firmware.

Instead of the UART, the USB test pads DM/DP (plus GND and power) reach the C3's USB Serial/JTAG. It works as a
flashing port with `idf.py flash` or esptool.

## Native USB variant

The firmware also builds for the ESP32‑C3's native USB Serial/JTAG (GPIO18 D−, GPIO19 D+). Line 0 then runs over USB
instead of UART0, and the host sees `/dev/ttyACM*` (VID 303A, PID 1001). On the reference board USB is only on test
pads. The USB variant builds without warnings but has not been tested on hardware.
