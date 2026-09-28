# IP bridge protocol (contract), version 1

This document covers the IP bridge run by the host daemon `host/espwifi_bridge.py` (SLIP frames). Controlling the
module directly over Kogger SBP (`ID_WIFI` 0x57, `ID_WIFI_NET` 0x58) is described in [SBP_WIFI.md](SBP_WIFI.md). Both
protocols share one port, see SBP_WIFI.md §5. While line 0 relays to the network ([RELAY.md](RELAY.md)), the port
understands only SBP and this protocol is unavailable. On factory settings (0.13) line 0 relays in either role:
switch it off first (`ID_WIFI_NET` v3).

The contract covers two links: the ESP32‑C3 module ↔ the host, and the host daemon ↔ applications. Implementations: firmware
`firmware/main/{frame,proto,manager}.c`, host `host/wbframe.py`, `host/espwifi_bridge.py`. `tests/test_all.py` checks
that the two framing implementations agree.

## 1. Layers

```
application ──TCP 127.0.0.1:5550, lines──▶ espwifi_bridge.py ──serial link, frames──▶ ESP32‑C3 ──Wi‑Fi STA──▶ access point
               (§4)                         TUN espwifi0 10.99.0.2     (§2, §3)              10.99.0.1, NAPT
```

## 2. Frame on the serial link

```
0xC0 | SLIP-escape( type[1] | data[n] | crc16[2, high byte first] ) | 0xC0
```

- SLIP escaping (RFC 1055): `0xC0 → DB DC`, `0xDB → DB DD`. Any other byte after `0xDB` is an error and the frame is
  dropped.
- CRC‑16/CCITT‑FALSE (polynomial 0x1021, initial value 0xFFFF, no reflection, no final XOR) over `type + data`.
- `n` ≤ 1500. Longer frames, frames with a bad CRC or shorter than 3 bytes are dropped silently. Junk between
  delimiters, such as the ROM bootloader's log at reset, is dropped the same way.
- Types: `0x01` one complete IPv4 packet (IPv6 and anything else is dropped); `0x02` one UTF‑8 control line without a
  line feed.

Transport: the C3's own USB Serial/JTAG (`/dev/ttyACM*`, VID 303A PID 1001) or UART (921600 8N1 by default, no flow
control). The host keeps **both** DTR and RTS asserted, otherwise an auto‑reset circuit would restart the module. The
same rule is assumed for USB Serial/JTAG; this has not been checked on hardware.

## 3. Control lines (host ↔ ESP)

Request: `<tag> <COMMAND> [key=value ...]`. Answer: `<tag> OK [fields]`, `<tag> ERR <CODE> [text]`, or intermediate
lines `<tag> NET|KNOWN ...` before the final `OK`.
- The tag is any token without spaces, up to 15 characters; the module silently truncates longer ones.
- The daemon adds its own prefix `c<N>.`, so an application has 15 minus the prefix length (8 with client numbers up to
  99 999).
- Unsolicited events start with `*`.

Fields are separated by spaces.
- Values are percent‑encoded: bytes `0x00–0x20`, `%` and `0x7F–0xFF` → `%XX`.
- The key is separated from the value by the **first** `=`.
- An SSID is bytes (up to 32), not necessarily UTF‑8.

| Command | Arguments | Answer |
|---|---|---|
| `VER` | — | `OK fw=0.11.0 proto=1 mac=aa:bb:.. idf=v5.x reset=<reason> baud=<rate> part=ota_0\|ota_1 ota=valid\|pending\|…` |
| `STATUS` | — | `OK` + state fields (below) |
| `SCAN` | — | `NET ssid= bssid= rssi= ch= auth= known=0\|1` for each network, then `OK count=N`; 2–4 s. `ERR BUSY` while connecting and in the AP role |
| `CONNECT` | `ssid=` (1–32 bytes), `pass=` (0–64), `save=0\|1` (default 1) | `OK` at once, then `* STATE` events. The network is saved only after an IP address is obtained |
| `DISCONNECT` | — | `OK`; auto‑connect is off until `CONNECT` or `AUTO` |
| `AUTO` | — | `OK`; look for saved networks again |
| `LIST` | — | `KNOWN ssid=` for each saved network, `OK count=N` (passwords are never sent) |
| `FORGET` | `ssid=` | `OK` / `ERR NOT_FOUND`; if it is the current network, disconnect and look for others |
| `STATS` | — | `OK link_rx= link_crc= link_disc= link_ovf= link_tx= link_txdrop= sbp_rx= sbp_ckerr= ip_in= ip_out= ip_bad= dns= dns_servfail= sta_rx= sta_tx= heap= uptime=` (`sta_*`: bytes of Wi‑Fi interface frames since boot) |
| `BAUD` | `rate=` 9600…5000000 | `OK`, then the switch (the answer goes out at the old rate); not saved |
| `PROTO` | `link=sbp\|slip` | `OK`; `sbp` switches the port to Kogger SBP until the next reboot |
| `RADIO` | `power=` 2…21 dBm (above 20 is stored as 20), `mode=b\|bg\|bgn\|lr\|bgnlr`, `bw=20\|40`, `ps=none\|min\|max` (any of them) | no arguments: show; `OK power= mode= bw= ps= power_now= phy= proto_now= bw_now=` (as `ID_WIFI` v7) |
| `RELAY` | `mode=off\|udp\|tcp addr= ip= port= lport=` (any of them) | no arguments: show; `OK mode= addr= ip= port= lport= state= peer= up_frames= … local=` (line 0, as `ID_WIFI` v5/v6). A bad value → `ERR BAD_ARGS` (0.11). After enabling, the port switches to SBP |
| `REBOOT` | — | `OK`, restart after 0.3 s |

Error codes:
- `BAD_LINE`, `BAD_ARGS`, `BUSY`, `NOT_FOUND`;
- `ABORTED`: a scan interrupted by CONNECT or a reconnect, or not finished within 15 s;
- `FAIL`;
- `ROLE`: `CONNECT`/`DISCONNECT`/`AUTO` in the AP role;
- `UNKNOWN_CMD`;
- the daemon adds `LINK_DOWN`.

State fields (in `STATUS` and `* STATE`):
`state=IDLE|SEARCHING|CONNECTING|CONNECTED|AP auto=0|1 ssid= bssid= ch= rssi= ip= gw= dns= reason=<ESP-IDF code> why=NONE|NO_AP|AUTH|ASSOC|LOST|OTHER rx_bps= tx_bps= role=sta|ap clients=`
- `rx_bps`/`tx_bps` are Wi‑Fi interface traffic in bytes per second over the last full second.
- In the AP role, `ssid`/`ch` are the module's own AP, `rssi` is the weakest client's, and `clients` is the client count.

Events:
- `* HELLO fw= proto= mac= reset=` after the module starts. `reset` = `POWERON|EXT|SW|PANIC|INT_WDT|TASK_WDT|WDT|BROWNOUT|USB|JTAG|OTHER`,
  the cause of the last reset. The same cause is in `VER` (`reset=`) and, over SBP, in `ID_WIFI` v1 byte 22; the
  console is not wired out in the UART variant.
- `* STATE ...` on every state change.
- `* CONNECT_FAILED ssid= reason= why=` after all attempts have failed; with `why=AUTH` for a new network, at once.
- `* AP_CLIENT event=joined|left|address mac= ip=` in the AP role.

In the AP role the module sends text only after the host has spoken SLIP first (0.11), because a device may sit on X1.

Behaviour:
- At start, if there are saved networks, the module scans every 15 s and connects to the strongest known one.
- A lost link → retry the same network; after 3 failures → search again.
- Up to 8 saved networks; when full, the least recently used one is replaced.
- Hidden SSIDs connect only through an explicit `CONNECT`.
- Since 0.11 the module leaves stuck states by itself:
  - a connection attempt longer than 30 s → search again;
  - association lost without a disconnect event (2 s without association) → reconnect;
  - a scan longer than 15 s → reset.

## 4. Host daemon (application ↔ daemon)

TCP `127.0.0.1:5550`, lines ending in `\n`. The line format is the same as in §3.
- The daemon replaces the tag with `c<N>.<tag>` towards the ESP and restores it in the answer, so clients do not
  interfere.
- `*` events go to all clients.
- The daemon answers `<tag> LINK` itself: `OK state=UP|DOWN to_esp= from_esp= dropped= opens= crc= disc=`.
- It sends `* LINK state=UP|DOWN` events, also right after a client connects.

## 5. Network

- Point‑to‑point link: ESP `10.99.0.1`, host `10.99.0.2` (/30), MTU 1500. The ESP does NAPT from the link into the Wi‑Fi
  station.
- Host: default route via `10.99.0.1` with metric 700. A wired Ethernet from NetworkManager has 100 and stays primary.
- DNS: `10.99.0.1:53` runs a relay to the DNS server received by DHCP from the access point. Without a connection it
  answers SERVFAIL at once.
- **Connections from the access point's network to the host do not get through** (NAT). Everything the host opens itself
  works: TCP, and UDP with the answer on the same port. The host does not see broadcasts of the boat network.
