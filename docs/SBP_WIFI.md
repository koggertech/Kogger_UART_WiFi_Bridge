# The module as a Kogger SBP device

Contract version 3 (firmware 0.11.0; version 2 = 0.10.0, version 1 = 0.2.0). Changes in 0.11 are summarised at the end
of §3 and §4. Implementation: `firmware/main/sbp.c`, `sbpdev.c`, `manager.c`, `netctl.c`. Python helpers (framing and most payloads): `host/sbpframe.py`.
Bench check: `tools/bench_sbp.py`.

A host opens the module's serial port (UART, or the C3's native USB) and talks plain Kogger SBP to it, as it would to a
sonar. The module answers its own IDs and the common device IDs. The same commands work over the second UART and over the
network ([NETWORK.md](NETWORK.md) §5).

## 1. Frame (KP1)

`BB 55 | route | mode | id | len | payload[len] | ck1 ck2`

- `ck1 ck2`: Fletcher‑8 over the bytes `route … payload`, the same checksum KoggerApp computes (`ProtoBinOut::end()`).
- `mode = type | ver<<3 | mark<<6 | resp<<7`. `type`: 1 CONTENT, 2 SETTING, 3 GETTING. Multi‑byte fields are
  little‑endian.
- A GETTING request is answered by a CONTENT of the same version, except `ID_WIFI` v3, which is answered with
  CONTENT v0. A CONTENT may grow in later firmware (new fields are
  only appended), so **parse CONTENT by minimum length, not exact length**.
- A SETTING request (with `resp` = 1, as KoggerApp sends it) is acknowledged by a CONTENT with `resp` = 1, the same
  version and the payload `{code, ck1, ck2}`. `ck1 ck2` is the checksum of the acknowledged SETTING frame, which is
  what KoggerApp compares in `IDBin::checkResponse()`. Codes: 1 OK, 3 ERR_PAYLOAD, 5 ERR_VERSION, 6 ERR_TYPE, 7 ERR_KEY,
  8 ERR_RUNTIME.
- IDs the module does not know are ignored silently, as sonars do. The `mark` bit is set after `ID_MARK` and cleared by
  a reboot and by `ID_BOOT` v0 (update window).

## 2. Address, identity, port

- **Address** (`route`): 0 by default. The module accepts SETTING and GETTING only with its own address. `ID_UART` v1
  changes the current address; v2 changes the boot address, which `ID_FLASH` v0 saves. While any UART line bridges to
  the network, the module uses its bridging address instead: 87 in the station role and 88 in the access‑point role
  by default ([NETWORK.md](NETWORK.md) §4).
- **Replies go where the request came from.** Acknowledgements and answers go back on the request's channel: the host
  port (UART0 or USB), UART1, or the network sender, from the same UDP port. Scan results (v2) go to whoever started
  the scan. Unsolicited frames go to the channel of the last request: state changes (v0), the periodic report (v1), v7
  after a connection and update progress. By default that is the host port in the station role, and nowhere in the
  access‑point role until someone asks.
- **`ID_VERSION` (0x20):**
  - v0, 34 bytes: `[1]` = board **87**, `[14..17]` = serial number (lower 4 bytes of the MAC);
  - v1, 12 bytes: UID (MAC followed by zeros);
  - v2, 9 bytes: `bootMode (0 firmware, 1 while the update window or a transfer is open), boardMinor 0, board 87, 0, 0,
    U2 0, fwMinor, fwMajor`.
- **`ID_UART` (0x18)**, KoggerApp's `IDBinUART` layout. SET v0 `{KEY 0xC96B5D4A, U1 1, U4 baud}`.
  - Any rate from 9600 to 4 000 000 is accepted, including all standard ones (9600 … 921600, 1 000 000, 1 200 000,
    1 500 000, 2 000 000, 2 500 000, 3 000 000, 3 500 000, 4 000 000).
  - The acknowledgement goes out at the old rate, then the module switches.
  - The rate is saved only by `ID_FLASH` v0 and applied at boot, as on sonars. The default is 921600, which is on
    KoggerApp's auto‑detect list.
  - GET: v0 → `{KEY, 1, baud}`, v1 → `{KEY, 1, address}`, v2 → `{KEY, boot address}`.
  - Since 0.10, v0 refers to the UART behind the request's channel. A request over UART1 or to line 1's UDP port
    sets line 1's rate, which is saved at once without `ID_FLASH`. Every other request sets line 0's rate.
- **`ID_MARK` (0x21):** SET v0 `{KEY}`; GET v0 → `{U1 mark}`.
- **`ID_FLASH` (0x23):** SET `{KEY}`. v0 saves the rate, the boot address and the report period; v1 reloads the saved
  boot address and report period (the rate in use is not changed: the saved rate applies at the next boot); v2 erases
  them.
- **`ID_BOOT` (0x24):** SET v0 `{KEY}` reboots the module after a 5 s update window.
  - A reboot brings back the **saved** rate and address (`ID_FLASH` v0), as on sonars.
  - Only an update, and a role change with reboot, carry the current rate and address over one boot.
- **Firmware update is accepted only over a wired line** (UART or native USB, 0.11). `ID_UPDATE` and `ID_BOOT` v1 from the network get
  ERR_RUNTIME. SBP has no authentication and the image is not signed ([UPDATE.md](UPDATE.md), [SECURITY.md](../SECURITY.md)).

2, 3 and 4 Mbaud were verified with `ID_UART` v0 on the UART of an RK3588 head unit. KoggerApp's "Set baudrate" list
(`DeviceSettingsPage.qml`) ends at 2 000 000 as of September 2026; a host that needs a higher rate sends `ID_UART`
itself.

## 3. `ID_WIFI` = 0x57

| Version | GETTING | SETTING |
|---|---|---|
| v0 | state (CONTENT v0) | — (ERR_TYPE) |
| v1 | link report (CONTENT v1) | `{U2 period_ms}`: 0 off, otherwise 100…60000 (clamped, no error); then CONTENT v1 |
| v2 | start a scan → one CONTENT v2 per network | the same, plus an OK acknowledgement; ERR_RUNTIME in the AP role |
| v3 | state (CONTENT v0) | connect `{U1 flags, U1 len, SSID, U1 len, password}`; ERR_RUNTIME in the AP role |
| v4 | saved networks → one CONTENT v4 each | action `{U1 code, …}`; codes 0 and 1 are ERR_RUNTIME in the AP role |
| v5 | relay settings of line 0 (10 bytes) | `{U1 mode 0/1 UDP/2 TCP, U1 own address, ip[4], U2 peer port, U2 own port}`, see [RELAY.md](RELAY.md) |
| v6 | relay statistics of line 0 (47 bytes) | — (ERR_TYPE) |
| v7 | radio: power and mode (8 bytes) | `{U1 power in 0.25 dBm 8…80, U1 protocols, U1 bandwidth 1/2, U1 power save 0/1/2}` |

**CONTENT v0: state** (39 bytes + SSID). Sent on request and **by itself on every state change**.

| Offset | Field |
|---|---|
| 0 | U1 state: 0 IDLE, 1 SEARCHING (looking for saved networks), 2 CONNECTING, 3 CONNECTED, 4 AP (access‑point role, AP running) |
| 1 | U1 flags: bit0 auto‑connect, bit1 scan running, bit2 current network is saved |
| 2 | S1 RSSI, dBm (0 when not connected) |
| 3 | U1 channel |
| 4 | U1 security: 0 OPEN, 1 WEP, 2 WPA, 3 WPA2, 4 WPA/WPA2, 5 WPA3, 6 WPA2/WPA3, 7 ENTERPRISE, 8 OWE, 255 other/unknown |
| 5 | U1 last disconnect reason (ESP‑IDF `wifi_err_reason_t`) |
| 6 | U1 category: 0 NONE, 1 NO_AP (network not found), 2 AUTH (wrong password), 3 ASSOC, 4 LOST, 5 OTHER |
| 7 | U1 number of AP clients (AP role); 0 in the station role |
| 8–13 | BSSID |
| 14–17 / 18–21 / 22–25 / 26–29 | IP / netmask / gateway / DNS, bytes a.b.c.d |
| 30–33 / 34–37 | U4 receive / transmit traffic, **bytes per second** |
| 38 | U1 SSID length, then the SSID (up to 32 bytes, not necessarily UTF‑8) |

In the AP role, v0 describes the module's own access point:
- SSID and channel are the AP's own;
- security is 0 OPEN, 3 WPA2 or 6 WPA2/WPA3;
- BSSID is the AP's MAC;
- IP and netmask are the AP's address, the gateway equals the AP address, DNS is 0;
- RSSI is that of the weakest client (0 without clients).

**CONTENT v1: link report** (23 bytes since 0.11, 22 before). Sent **by itself once per period** (1000 ms by default)
while the channel is in SBP mode: `U1 state, S1 RSSI, U4 rx_Bps, U4 tx_Bps, U4 rx_total, U4 tx_total, U4 uptime_s,
U1 last reset reason`. Totals wrap at 2³². In the AP role RSSI is that of the weakest client.

The reset reason is ESP‑IDF's `esp_reset_reason_t`: 1 power‑on, 3 software (`esp_restart`: update, role change, the
`ID_BOOT` window), 4 panic, 5 interrupt watchdog, **6 task watchdog**, 7 other watchdog, 9 brownout, 14 power glitch,
15 CPU lockup, 0 unknown. An uptime that drops to zero shows a restart; this byte shows its cause without the text console.

**Traffic** counts the bytes of frames that the module's Wi‑Fi interface received and sent (Ethernet header + IP
packet, as lwIP sees them) over exactly 1 s. It is the real Wi‑Fi traffic, whatever data path runs on top.

**CONTENT v2: one network from a scan** (13 bytes + SSID), sorted by descending RSSI: `U1 index, U1 total, S1 RSSI,
U1 channel, U1 security, U1 flags (bit0 saved, bit1 current), BSSID[6], U1 len, SSID`. No networks: one frame `{0, 0}`.
- A scan takes about 2–4 s.
- A scan is impossible during a connection attempt: the answer is an ERR_RUNTIME acknowledgement.
- If the scan is aborted (by a connection or a driver reconnect) or does not finish within 15 s, the same
  ERR_RUNTIME comes instead of a partial list (0.11).
- The results, and the ERR_RUNTIME of an aborted scan, go only to the channel that started the scan. A second
  SETTING v2 from the same channel during a running scan is acknowledged with OK and shares the results; from another
  channel it gets OK but no results. A second GETTING v2 has no acknowledgement of its own: from the same channel it
  receives the results, from another channel nothing.

**SETTING v3: connect.** Flags bit0 = save the network; it is saved only after an IP address is obtained. SSID 1–32
bytes, password 0–64. The OK acknowledgement comes at once; the progress shows in CONTENT v0. On failure the state
goes to SEARCHING or IDLE with the reason code and category (wrong password → AUTH). Hidden networks connect only
through an explicit v3.

**CONTENT v4: one saved network**: `U1 index, U1 total, U1 len, SSID`; none: `{0, 0}`. Up to 8 networks. Passwords are
never sent out. **SETTING v4: action**:
- `{0}` disconnect and turn auto‑connect off;
- `{1}` turn auto‑connect on;
- `{2, U1 len, SSID}` forget a network (unknown → ERR_PAYLOAD);
- `{3}` forget all.

**v7: radio** (firmware 0.8.0). SETTING, 4 bytes:

| Field | Values |
|---|---|
| U1 power | transmit power limit in 0.25 dBm, 8…80 (2…20 dBm); 81…84 are accepted and stored as 80 |
| U1 protocols | 0x01 b, 0x03 b/g, 0x07 b/g/n, 0x08 LR, 0x0F b/g/n + LR (LR is Espressif's long‑range mode) |
| U1 bandwidth | 1 = 20 MHz, 2 = 40 MHz (only together with 11n) |
| U1 power save | 0 off, 1 MIN_MODEM, 2 MAX_MODEM |

CONTENT v7 (on GETTING, after SETTING, and since 0.11 by itself after every connection and AP start), 8 bytes: the
same 4 fields, then `U1 power in effect (0.25 dBm), U1 mode negotiated with the AP (0 LR, 1 11b, 2 11g, 4 HT20, 5 HT40,
0xFF no link), U1 protocols set in the driver, U1 bandwidth set in the driver`.

- **The real channel width is byte 5** (4 = HT20, 5 = HT40). Bytes 6–7 report what the driver interface is set to
  (`esp_wifi_get_protocol/bandwidth`), not what was negotiated. A module set to 40 MHz shows 40 in byte 7 while the
  link runs HT20 (verified in the field). Since 0.11 the module compares bytes 6–7 with the setting after every start
  and connection, and re‑applies the setting if they differ.
- A CONTENT v7 right after a SETTING that changes protocols or bandwidth carries the state **before** the reconnect
  (`phy` = 0xFF). The real state follows by itself after CONNECTED (0.11).
- **Power.** The driver applies power in steps: 2, 5, 7, 8.5, 11, 13, 14, 15, 16.5, 18, 20 dBm
  (`esp_wifi_set_max_tx_power`, ESP‑IDF `esp_wifi.h`). That is why the module reports both the set and the applied
  value. Power is re‑applied after every connection, because country information from the AP may lower it.
- **Protocols and bandwidth** take effect at the next connection, so the module reconnects to the same network at once.
- **LR** works only with an access point on an Espressif chip with LR enabled. The combination "b/g/n + LR" has not
  been tested on hardware.
- The settings are saved in NVS and applied at power‑up. Defaults: 20 dBm, b/g/n, 20 MHz, no power save. The driver does
  not go above 20 dBm, so 0.11 stores 80 instead of 84 and "set" equals "in effect".
- Text command (SLIP mode, bench): `RADIO` shows; `RADIO power=15 mode=bgn bw=20 ps=none` sets.
- **In the AP role** (0.10) protocols and bandwidth apply to the access point, and changing them restarts it 0.2 s after
  the answer (0.11). Power save is not applied: an access point must listen to its clients. The negotiated mode is
  0xFF. "LR only" (0x08) is refused in the AP role (ERR_PAYLOAD, 0.11; the text `RADIO` command does not check it, known issue): no phone or laptop could join such an AP, and
  the setting could then only be undone over a wire.

![TX power steps](img/tx_power_steps.svg)

**Changes to `ID_WIFI` in 0.11:**
- v1 is one byte longer (reset reason);
- v7 comes by itself after a connection, bytes 6–7 are named correctly, power goes up to 80;
- LR‑only is refused in the AP role;
- v2 answers whoever asked and never returns a partial list;
- v5 with mode "off" no longer touches the module's address, and enabling with address 0 is refused.

## 4. `ID_WIFI_NET` = 0x58 (firmware 0.10.0)

Role, access point, address and DHCP, UART lines and their ports. The model is in [NETWORK.md](NETWORK.md).

- **Every SETTING starts with the key** `U4 0xC96B5D4A`; without it the answer is ERR_KEY. It works like `ID_UART`,
  because these settings can cut the module off.
- Every SETTING is acknowledged, then the module sends a CONTENT with what is now in effect.
- Settings are saved to NVS at once.

| Version | GETTING | SETTING (after the key) |
|---|---|---|
| v0 | role: `{U1 at next boot, U1 now}`, 0 station, 1 AP | `{U1 role, U1 flags}`: bit0 reboot right after the answer, bit1 forget lines and own address (the new role starts from its defaults) |
| v1 | access point: `{U1 channel 1–11, U1 hidden 0/1, U1 max clients 1–10, U1 security 0 open/1 WPA2/2 WPA2+WPA3, U1 len, SSID, U1 0}` | the same, ending with `U1 len, password` (8–63; empty for open). An empty password with security on keeps the old password |
| v2 | address: `{ip[4], mask[4], U1 DHCP, pool start[4], pool end[4], U2 lease min, U1 offer: bit0 gateway, bit1 DNS}` (20 bytes) | the same 20 bytes |
| v3 | line (18 bytes): `{U1 line, U1 mode 0 off/1 UDP/2 TCP, U1 dest 0 fixed/1 senders/2 broadcast, ip[4], U2 rport, U2 lport, U4 baud, S1 TX, S1 RX, U1 uart}`. No payload: both lines; `{U1 line}`: one | the first 17 bytes of the record. For line 0 baud and pins are ignored (rate: `ID_UART`; pins: build) |
| v4 | line statistics (57 bytes); no payload: both, `{U1 line}`: one | — (ERR_TYPE) |
| v5 | AP clients: `{U1 index, U1 total, MAC[6], ip[4], S1 RSSI}` each; none: `{0, 0}` | — (ERR_TYPE) |
| v6 | own address while bridging `{U1}` | `{U1 address 1–255}` (0 → ERR_PAYLOAD, 0.11); acknowledged from the old address |

- **v0.** The role takes effect at boot. With the reboot flag, the module carries the current rate and address over one
  boot, as after an update, and it keeps that address for this boot even while a line bridges. It moves to its
  bridging address (the saved one, otherwise the role default: 87 station, 88 access point) after the next reboot or after any line setting (`ID_WIFI_NET` v3
  or v6, `ID_WIFI` v5).
  - **Known issue (0.11):** from the station default (address 0), the AP comes up with both lines relaying while the
    module still answers address 0, so it takes SETTING/GETTING frames meant for a device at address 0. Send an
    `ID_WIFI_NET` v6 (or reboot once more) right after the role change.
- **v1.** The network name can be changed at will.
  - To change only the name, take the fields from a GETTING v1, put in the new name and send it with an empty password.
    With security on, the password stays. Example for the name `Boat-1`, WPA2, channel 6, up to 4 clients:
    `4A 5D 6B C9 | 06 00 04 01 | 06 'B' 'o' 'a' 't' '-' '1' | 00`.
  - In the station role the setting is only saved and takes effect in the AP role.
  - In the AP role the AP restarts 0.2 s after the answer; clients disconnect and join again.
  - The password is never read back: the answer carries password length 0.
  - **Channel 1–11** (0.11; 0.10 allowed 1–13). The driver's default country ("01") gives an AP no other channels.
    With channel 12–13 the driver would refuse the configuration, and after a reboot its own open AP could come up
    (inference from the ESP‑IDF documentation; the driver is closed source). 0.10 records with channel 12–13 are
    clamped to 11 when 0.11 boots.
  - If the driver still refuses a configuration, 0.11 restores the previous one; at boot it uses the built‑in
    defaults. If even those are refused, the radio does not start at all. There is never an open AP.
- **v2.** Validation rules: [NETWORK.md](NETWORK.md) §3. In the AP role the change applies 0.2 s after the answer, and
  all clients are disconnected so they take a new lease.
- **v3.** `uart`: 0 UART off, 1 running on these pins, 2 the saved pins take effect after a reboot. For line 0 the answer
  carries the current rate and the build's pins (−1/−1 in the USB variant). An error in any field → ERR_PAYLOAD and
  nothing changes.
- **v4.** The payload starts with `{U1 line, U1 state 0 off/1 no Wi‑Fi/2 no peer/3 ready, ip[4], U2 peer port,
  U1 senders}`, then twelve U4 counters:
  - up: frames, bytes, packets, drops;
  - down: packets, bytes, frames, drops;
  - frames addressed to the module, TCP connections, UART receive overflows, UART transmit drops.

  The peer is the fixed/TCP peer, the broadcast address or the last sender.
- **v6** is the same field as `addr` in `ID_WIFI` v5. Address 0 is refused: devices sit there by default, and the module
  would take their frames.
- An unknown version → ERR_VERSION (v7).

**Changes to `ID_WIFI_NET` in 0.11:**
- AP channel 1–11; 0.10 records with channel 12–13 are clamped to 11;
- a configuration the driver refuses falls back to the previous one, at boot to the defaults, else the radio stays
  off: never an open AP;
- v6 with address 0 → ERR_PAYLOAD;
- v0 with reboot: the address in use is carried over one boot even while a line bridges (0.10 switched to the
  bridging address); see the known issue under v0;
- a disabled DHCP server stays disabled after an AP restart.

## 5. One port, two protocols

The same port also understands SLIP frames for the IP bridge ([PROTOCOL.md](PROTOCOL.md), daemon `host/espwifi_bridge.py`).
- The first valid frame after power‑up locks the protocol until the next reboot: an SBP frame with the module's
  address selects SBP, a SLIP frame selects SLIP.
- In SBP mode the module is silent on SLIP, and in SLIP mode on SBP.
- Until the protocol is locked, in the station role the module sends SLIP text notifications (`* HELLO` at boot,
  `* STATE` on changes). KoggerApp's parser skips those bytes as noise.
- In the AP role (0.11) no text is sent before the host locks SLIP, because a device may sit on X1.
- The text command `PROTO link=sbp` switches SLIP → SBP without a reboot.
- While line 0 bridges (0.7+), the port is locked to SBP from the start.

## 6. ID choice

`ID_WIFI` 0x57, `ID_WIFI_NET` 0x58 and board ID 87 were unused in KoggerApp when they were chosen
(September 2026). 0x57 ("W") sits in the middle of the empty range 0x41–0x63, away from the groups KoggerApp extends
one by one; 0x58 is its neighbour.
