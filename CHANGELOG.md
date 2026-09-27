# Changelog

Newest first. `ID_VERSION` carries only major.minor, so every release raises the second number
([docs/UPDATE.md](docs/UPDATE.md)). "Verified" means checked on hardware.

## Hardware — 2026‑09‑27

KiCad 10 project of the reference board v3 in `hardware/`: schematic, board, libraries, fabrication outputs. Its
drill holes and solder‑paste openings match the fabrication files of the manufactured board
([hardware/README.md](hardware/README.md)).

## 0.11.0 — 2026‑09‑27

A fix release after a full review of the project. Every finding was checked against the code or the ESP‑IDF 5.5.5 source.
Builds (UART and USB) without warnings; PC tests 43/43. **Field:** installed on a head unit's module over SBP at
3 Mbaud, 7/7 checks (932 512 bytes in 50 s, confirmed after 65 s and a proving reboot), all settings kept. Not yet
verified: the transmit fix under a saturated 921600 port.

Firmware:
- **UART transmit without a driver ring.** With a ring, `uart_write_bytes()` polled for space without sleeping, the
  transmit tasks starved all lower priorities, and the task watchdog rebooted the module under a saturated port. Root
  cause of field restarts at 921600 baud (the link is an inference). USB variant: writes over 4096 bytes never went
  through.
- Receive reads everything buffered; the event queue is reset before the input is flushed on overflow.
- **AP channel 1–11.** Channel 12–13 was saved with OK and could bring up the driver's own open AP after a reboot. A
  refused configuration now falls back to the previous one or to the defaults; without them the radio stays off.
- **A new image confirms itself after 60 s of work** (was 5 s); rollback deadline 180 s (was 120 s).
- **Firmware updates only over a wire:** `ID_UPDATE` / `ID_BOOT` v1 from the network get ERR_RUNTIME.
- A disabled DHCP server stays disabled after an AP restart; 10 leases for 10 clients; the lwIP UDP queue is 16.
- Manager queue slots reserved for Wi‑Fi events; supervision of stuck station states; driver return codes checked;
  NVS written on reconnect only when the network changes.
- Address 0 refused as the module's address; `ID_WIFI` v5 "off" keeps the address; the one‑boot address survives the
  start; the route is set before the port starts.
- Rescan budget in the relay framer: a 64 KB stream of false syncs costs 9.5× instead of 660× (rescanned bytes per
  input byte).
- A frame longer than 512 B gets all its pool slots or is not sent; the last 1 KB of each UART transmit ring is kept for
  the module's own frames; TCP waits up to 5 s for window space.
- `ID_WIFI` v1 reports the last reset reason (23 bytes); v7 is sent after every connection, bytes 6–7 are named
  correctly and re‑checked against the driver; power up to 20 dBm stored as 80; LR‑only refused in the AP role; scan
  results go to whoever asked and never as a partial list.
- Smaller fixes: an update session never inherits the previous one's state; `ID_BOOT` v1 after an abandoned transfer
  answers ERR_RUNTIME; bridge DNS in the AP role; no text on X1 in the AP role before SLIP; `RELAY` with a typo refused;
  NUL in "forget network" refused; settings change RAM only after NVS succeeds; the 0.7 record migrates only once.

Known issues:
- After a role change with reboot, the module keeps its previous SBP address for that boot even while a line relays;
  from the station default (address 0) it then takes frames meant for a device at 0 until an `ID_WIFI_NET` v6 or
  another reboot ([docs/SBP_WIFI.md](docs/SBP_WIFI.md) §4).
- The text command `RADIO mode=lr` does not refuse "LR only" in the AP role (the SBP path does).

Tools:
- `sbp_update.py` read the acknowledgement of `ID_BOOT` v0 instead of v1 and always reported OK. `--sbp-only` now
  verifies the proving reboot and waits for the 60 s confirmation.
- `fwinfo.py` hands the port back to SBP.
- `bench_relay.py` and `bench_sbp.py` restore every setting they change.
- The bridge daemon survives a module unplug.
- A new PC test streams 60 KB of false syncs through both framers.

## 0.10.0 — 2026‑09‑26

- Station or access‑point role, chosen at boot; access point with name, password, security, channel, client limit;
  address and DHCP server settings.
- Two UART lines (X1, X2), each mapped to its own UDP port or TCP client; destinations fixed / senders / broadcast.
- `ID_WIFI_NET` 0x58; the module is controllable from any line and from the network; replies go to the request's
  channel.
- TCP connect in the background of the network `select`.
- Fix of the 0.7/0.8 reboot loop with the relay off and Wi‑Fi connected.
- `sbp_update.py --sbp-only` for ports that speak only SBP.
- **Verified in the field:** the head unit's module updated from 0.7.0 over SBP (5/5), the relay record migrated to line 0,
  and 2/3/4 Mbaud were set over SBP. Not verified: AP role, line 1.

## 0.8.0 — withdrawn

Radio control (`ID_WIFI` v7: power, protocols, bandwidth, power save). Withdrawn because of the 0.7/0.8 reboot loop
(relay off + Wi‑Fi connected); the features continue in 0.10.0. Version 0.9 is used by a test image.

## 0.7.0 — 2026‑09‑25

- Frame‑aware relay between the host port and UDP/TCP: Kogger KP1/KP2, UBX, MAVLink 1/2 recognised by their checksums;
  packets ≤ 512 B of whole frames; raw pieces ≤ 512 B; silence flush with rescan.
- `ID_WIFI` v5/v6: relay settings and statistics.
- **Verified in the field:** relay to a boat network over UDP without loss.

(0.6.0, not released: the first relay version, Kogger SBP only.)

## 0.4.0 / 0.3.x — 2026‑09‑25

- Firmware update over Kogger SBP with KoggerApp's exchange: two OTA slots, image validation before the switch,
  rollback of an unconfirmed image, UART ISR in IRAM.
- **Verified:** update 0.3.0 → 0.3.1 (10.8 KB/s), recovery from a lost chunk, a corrupted image refused.

## 0.2.0 — 2026‑09‑25

- The module as a Kogger SBP device: `ID_WIFI` 0x57, board ID 87, common IDs (`ID_VERSION`, `ID_UART` 9600 … 4 000 000,
  `ID_MARK`, `ID_FLASH`, `ID_BOOT`); Wi‑Fi traffic statistics; one port for SBP and SLIP.
- **Verified:** SBP checks 16/16, baud switching 28/28 up to 2 Mbaud, IP bridge regression 11/11.

## 0.1.0 — 2026‑09‑25

- IP bridge: SLIP + CRC‑16 frames, text commands, point‑to‑point lwIP interface, NAPT into the Wi‑Fi station, DNS relay;
  Linux daemon with TUN.
- Station with up to 8 saved networks and auto‑connect.
- **Verified:** smoke test 11/11, connection to a boat access point, ping through NAPT, auto‑connect after a power cycle.
