# Role, access point, network and UART lines

Firmware 0.10.0, fixes in 0.11.0. Implementation:
- `netcfg.c`: settings and their validation;
- `wifiap.c`: access point and DHCP server;
- `uline.c`: UART1;
- `relay.c`: both lines ↔ network;
- `netctl.c`: `ID_WIFI_NET` 0x58.

The contract is in [SBP_WIFI.md](SBP_WIFI.md) §4 and the relay buffers in [RELAY.md](RELAY.md). Board connectors:
[HARDWARE.md](HARDWARE.md).

## 1. Lines

- **Line 0** = X1 = UART0: the host port (KoggerApp, the bridge daemon) or a device port.
- **Line 1** = X2 = UART1, default pins TX 5 / RX 4. For another board the pins can be set over SBP, from
  {0, 1, 3, 4, 5, 6, 7, 10}. New pins take effect after a reboot: a running UART is not reconfigured.

## 2. Role

| Role | What the module does |
|---|---|
| station (default) | joins an existing network, e.g. a boat's access point |
| access point | runs its own network: name, password, channel, address, DHCP |

- The role is stored in NVS and applied at boot. It is changed with `ID_WIFI_NET` v0, with or without the "reboot now"
  flag.
- Both roles at once (APSTA) are not supported, to keep things simple. The ESP32‑C3 has one radio, and in APSTA the
  access point would have to follow the station's channel.
- In the AP role there is no station. Scan, connect, and the v4 actions "disconnect" and "auto‑connect" answer
  ERR_RUNTIME. The list of saved networks is kept for the return to the station role.

**Access point defaults:** name `Kogger-XXXX` (XXXX = last two bytes of the AP's MAC), WPA2, password `kogger1234`,
channel 6, up to 4 clients, name visible. **The default password is public: change it when you first set up an access
point** ([SECURITY.md](../SECURITY.md)). The password is never read back.

**Channel 1–11** (0.11; 0.10 accepted 1–13).
- The driver's default country ("01", channels 1–11) always applies to an access point, and the driver refuses an AP
  channel outside it (`ESP_ERR_INVALID_ARG`, ESP‑IDF documentation, "Wi‑Fi Country Code"; an inference, the driver is
  closed).
- In 0.10 such a setting was saved with OK. After a reboot the driver's own access point `ESP_xxxxxx` came up,
  apparently open, with both lines relaying.
- In 0.11 channel 12–13 is refused (ERR_PAYLOAD), and 0.10 records with such a channel are clamped to 11 at boot.
- If the driver refuses a configuration at run time, the previous one comes back; at boot, the built‑in defaults.
- If even the defaults are refused, the radio does not start at all. There is never an open access point, and the
  module stays reachable over UART.

**Name, password and the rest can be changed at will.** `ID_WIFI_NET` v1 sets the name (1–32 bytes), password,
security, channel, hidden flag and client limit in one frame.
- To change only the name, leave the password empty: with security on, the old password stays.
- The settings can be prepared in the station role. They are saved and take effect when the module switches to AP.
- A change restarts the access point, so clients disconnect and join again. ESP‑IDF does not document how a running AP
  takes new settings, so the module does it explicitly: stop → configure → start.

## 3. Address and DHCP of the access point

Defaults: **10.0.0.10/24**, DHCP on, pool 10.0.0.11–10.0.0.30, lease 120 min, no gateway or DNS offered to clients.

- The validation matches the ESP‑IDF DHCP server's own checks (`esp_netif_lwip.c`, IDF 5.5.5), so the module never
  accepts what the driver would refuse later:
  - the netmask is contiguous, neither /0 nor /32;
  - the AP address is neither the network nor the broadcast address;
  - the pool lies inside the subnet, start < end, does not contain the AP address, and has at most 100 addresses
    (the IDF server's limit);
  - the lease is 1–2880 min.
- Gateway and DNS are off by default because the module has no internet uplink. A phone that receives a gateway may send
  all its traffic there and lose mobile data (an assumption; common Android behaviour). Both can be enabled; the DNS
  offered is then the AP address, but the module runs no DNS server in the AP role.
- A change of address or DHCP applies at once. The module disconnects all clients so they take a new lease.
- **A disabled DHCP server stays disabled** (0.11).
  - In 0.10 a server disabled at boot came back with the default pool and gateway at every AP restart. esp_netif marks
    a never‑started server "to be started" when the interface stops (`esp_netif_lwip.c`).
  - 0.11 stops the server again after every AP start.
- The server holds up to 10 leases (`CONFIG_LWIP_DHCPS_MAX_STATION_NUM`, 0.11), as many as the AP admits. With the IDF
  default of 8, a ninth client got an address taken from the oldest lease.

## 4. Lines and ports

Each line maps to its own UDP port or to a TCP client:

| Field | Meaning |
|---|---|
| mode | off / UDP / TCP (client; meant for the station role) |
| dest (UDP) | **fixed**: to ip:rport. **senders**: to whoever sent to the local port, up to 4 senders, each forgotten after 60 s of silence. **broadcast**: to the subnet broadcast address on rport |
| ip, rport | peer for fixed and TCP |
| lport | local UDP port; 0 = same as rport |
| baud | line 1 only (line 0's rate is set by `ID_UART`), 9600 … 4 000 000 |
| TX/RX pins | line 1 only; −1 = UART1 off |

**Defaults:**

| Role | Line 0 (X1) | Line 1 (X2) | Own SBP address while bridging |
|---|---|---|---|
| station | off: host port | off; when enabled: UDP senders 14445, 115200, pins 5/4 | 87 |
| access point | UDP senders, port 14444 | UDP senders, port 14445 | 88 |

- A role's defaults apply while a line has not been saved. Saved lines are shared by both roles. The "forget lines"
  flag of a role change erases them together with the address, and the new role starts from its defaults.
- Two UDP lines cannot listen on the same port; such a setting is refused.
- The 0.7 relay setting (`ID_WIFI` v5) is a view of line 0. On the first boot of 0.10 a saved 0.7 record moves into
  line 0 (a switched‑off record does not). The old record stays for a rollback to 0.7.

**The module's own address.** While any line bridges, the module answers only its own address: 87 in the station role
and 88 in the AP role by default (`ID_WIFI_NET` v6 or `ID_WIFI` v5). Everything else from a line goes to the network,
so a device on the line keeps its own address; a sonar is usually 0. When no line bridges, the boot address (0)
applies. The different defaults 87/88 let a pair of modules on one link be told apart.
- Address 0 is refused as the module's address (0.11): devices sit there by default. `ID_WIFI` v5 with mode "off" no
  longer writes the address.
- The address in use (for example one set with `ID_UART` v1, or the default 0) survives an update or a role change
  with reboot for one boot, even when a line bridges (0.11). The bridging address takes over after the next reboot or
  after any line or address setting (`ID_WIFI_NET` v3/v6, `ID_WIFI` v5).
- **Known issue (0.11):** a role change with reboot from the station default (address 0) brings the AP up with both
  lines relaying while the module answers address 0 for that boot, taking SETTING/GETTING frames meant for a device
  at 0. Send `ID_WIFI_NET` v6 or reboot once more after the role change ([SBP_WIFI.md](SBP_WIFI.md) §4).

## 5. Control over any line and over the network

The module takes frames with its own address (SETTING/GETTING) from everywhere: UART0, UART1, and the network on the
port of any open line. It takes them even from a sender to which the line relays nothing (not its fixed peer). So the
module is reachable on any open port.

- The answer goes back where the request came from: the same UART, or the same sender from the same port.
- Unsolicited frames go to where the last request came from: state changes, the periodic v1 report and update
  progress. Scan results go to the channel that started the scan.
  - In the station role this is the host port by default.
  - In the AP role it is nowhere until someone asks, because a device on X1 has no use for the module's frames.
- `ID_UART` v0 (rate) refers to the UART behind the request's channel. A request over UART1 or to line 1's port changes
  line 1's rate, which is saved at once; others change line 0's rate.
- **Firmware updates from the network are refused** (0.11). `ID_UPDATE` and `ID_BOOT` v1 from a network peer get
  ERR_RUNTIME. The SBP key is public, the image is not signed, and anyone who knows the Wi‑Fi password can become a
  client. Firmware is changed only over a wire (X1 or X2). Reboot (`ID_BOOT` v0) and role changes still work from the
  network.

Example, AP role with a sonar on X1:
1. A laptop joins the module's network and sends SBP to 10.0.0.10:14444.
2. Frames with address 88 go to the module; frames with the sonar's address go out on X1.
3. The sonar's answers go to whoever sent.

Whether KoggerApp's UDP connection works this way (sends to the given address and listens for answers from it) has not
been checked.

## 6. Protections

- The module never sends to its own address. Otherwise a fixed line pointing at the AP's own address, e.g. after a
  station configured for peer 10.0.0.10 switches to the AP role with that address, would loop the stream back. Such a
  packet counts as lost and the line state is "no peer".
- Datagrams from the module's own address, such as a returning broadcast, are not written to a UART.
- Both RX lines have a weak pull‑up ([HARDWARE.md](HARDWARE.md)).
- "LR only" is refused in the AP role over SBP (0.11): no ordinary device could join such an AP. **Known issue:** the
  text command `RADIO mode=lr` (SLIP) does not check this and restarts the AP without the 0.2 s delay.
- In the AP role the module sends no text notifications on X1 until X1 is locked to SLIP (0.11): a device may sit there.

## 7. Verification status

- **0.10.0 in the field** (head unit): updated from 0.7.0 over SBP; the 0.7 relay record moved into line 0 (UDP fixed);
  line 1 off; station role, address 87. The boat stream flows, and all `ID_WIFI_NET` settings read back over SBP.
- **0.11.0 in the field** (head unit): updated from 0.10.0 over SBP; role, access point, addresses, lines, address 87
  and radio settings were all kept.
- **Not yet verified on hardware:** the 0.11 transmit fix under a saturated 921600 port; rollback of an unconfirmed image and an abandoned transfer; the access‑point role; UART line 1; writing `ID_WIFI_NET` settings; the native USB variant; the IP bridge daemon on a Linux host.
- `tools/bench_sbp.py` changes nothing permanently without flags. Writing line 1 needs `--write-test`, and the role
  change needs `--role-test`.

## 8. Open questions

1. The AP defaults (`Kogger-XXXX` / `kogger1234`) are placeholders. They can be changed at any time
   (`ID_WIFI_NET` v1). For a product, see [SECURITY.md](../SECURITY.md).
2. The LED on GPIO0 is unused. It could show role, link and traffic.
3. TCP: a packet may wait up to 5 s for window space. One shared send task can then hold the other line's packets for
   as long. UDP is not affected.
4. Changing protocols and bandwidth in the AP role restarts the AP. The driver settings are assumed to survive
   `esp_wifi_stop/start` (not verified).
