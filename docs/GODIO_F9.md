# GODIO F9 telemetry → MAVLink (line 1)

A GODIO boat has a serial wire from its controller to its radio receiver. On that wire the controller sends telemetry
frames (F9: position, heading, supply voltage, satellites) at 9600 8N1. A module whose line 1 runs the **input
translator** listens to this wire on X2 and never drives it. It passes on a translation into MAVLink 2, which line 1's
relay sends to its UDP peer like any MAVLink from a UART. A ground station that speaks MAVLink sees the boat without
changes; in KoggerApp that is the map, the track and the sonar mosaic.

The translator is in every build of this repository from 0.23: build option `WB_LINE1_GODIO_F9`, on by default
([PARAMETERS.md](PARAMETERS.md)). A module runs it only when line 1's input translator is set to 1, see [Enabling](#enabling).
Code: `firmware/main/f9.c` (frames), `f9mav.c` (MAVLink), `linexlat.c` (the line), mirrored in Python by `host/f9.py`.

## Wiring

| Boat | | Module |
|---|---|---|
| ground | → | X2 pin 3 (GND), connected first |
| telemetry wire, controller → receiver | → | X2 pin 1 (GPIO5, used as the line's RX) |
| | | X2 pin 2 not used; X2 pin 4 (VCC_X2) left unconnected |

- The module only listens. Line 1's TX is parked on GPIO10, which is on no header. Anything queued for the line is
  dropped and counted as UART transmit drops in the line statistics (`ID_WIFI_NET` v4). The link between controller and
  receiver is not disturbed.
- The module's inputs are 3.3 V CMOS ([HARDWARE.md](HARDWARE.md)): measure the wire's idle level before connecting it.
- On an unknown wire, `tools/gps_sniffer/` finds which pin carries data and at what rate. It is a small separate app
  for the same board and recognises GODIO F8/F9 frames, NMEA and UBX ([its README](../tools/gps_sniffer/README.md)).

## Enabling

Load the module profile of a GODIO boat:

```
python tools/profile.py --port COM3 apply profiles/boat_godio.json
```

The profile sets up the module and reboots it into these settings:

- **Module:** access point `KoggerBridge`, channel 6, WPA2; own address 88; X1 at 921600.
- **Line 0:** UDP to its senders, port 14444.
- **Line 1:** UDP to its senders, port 14445; 9600 baud, RX GPIO5, TX GPIO10, translator on.

Or set line 1 alone: `ID_WIFI_NET` v3 SETTING with an 18th byte of 1 ([SBP_WIFI.md](SBP_WIFI.md) §4), for example
`host/sbpframe.line_payload_from(record, xlat='telemetry')`. The translator starts with the next boot.
`tools/gps_sniffer/set_godio_ap.py` does this and switches to the access‑point role, for a bench.

While the translator runs, line 1 works at 9600 whatever rate is saved, and a change of its rate is refused. The line
record reports three bytes (18–20): the saved choice, the one running, and the translators the build carries.

**The profile's network is the published factory network** (name and password `KoggerBridge`). Anyone who knows it
can join, and from 0.17 update the module over the network ([SECURITY.md](../SECURITY.md)). A boat in use needs its
own network name and password.

## Frames on the wire

This is how the firmware decodes the wire. It was worked out from the vendor's example and from captures on a boat.

- A frame: a sync byte (`F8` command, `F9` telemetry), a type byte, data, and an 8‑bit sum of all previous bytes. There
  is no length field.
- **Boundaries.** At a sync byte, the frame is the shortest length of 4–40 bytes that meets both conditions:
  - its sum is valid;
  - another sync byte or the end of the data follows it.

  A sync byte with no such length is skipped as noise.
- **End of the data:** 20 ms of silence on the line. A shorter UART timeout can fall inside a frame, so it does not end
  the data.
- **Bytes fed in pieces.** The parser decides a frame only once the byte after it is known, so it finds the same frames
  as the rule applied to the whole stream.

**F9 C4, position** (19 bytes, little‑endian):

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `F9` |
| 1 | 1 | `C4` |
| 2 | 4 | longitude, float32, degrees |
| 6 | 4 | latitude, float32, degrees |
| 10 | 2 | distance from the start point, m |
| 12 | 1 | supply voltage, 0.1 V |
| 13 | 4 | heading, float32, degrees 0–360 |
| 17 | 1 | satellites |
| 18 | 1 | sum of bytes 0–17 |

**F9 C0, status** (19 bytes):
- bytes 2–11: status and GPS speed, not decoded;
- bytes 12–17: as in C4 (voltage, heading, satellites);
- byte 18: the sum.

Other types (F9 C5, the F8 commands) are cut out as frames but not translated.

Example: `F9 C4 33 0A F8 42 93 12 2A 42 00 00 7C 00 00 F0 41 26 18` decodes to:
- longitude 124.0199, latitude 42.5181;
- 12.4 V;
- heading 30.0°;
- 38 satellites.

Rates measured on a boat (October 2026):
- C4 at 1 Hz and C0 at 8 Hz;
- after an update of the boat's controller, both at 4.78 Hz;
- the heading value itself changes about 2–3 times a second.

## MAVLink out

MAVLink 2 frames:
- system and component 1/1, unsigned;
- base payload lengths: no extensions, no trimmed zeros;
- CRC_EXTRA from the firmware's table (`mavcrc.c`).

| When | Message | Content |
|---|---|---|
| each F9 C4 | `GPS_RAW_INT` (#24) | fix 3 with a valid position, else fix 1 with lat/lon 0; satellites. Course over ground, eph, epv and speed: unknown |
| each F9 C4 | `GLOBAL_POSITION_INT` (#33) | only with a valid position: lat/lon in degrees × 10⁷; `hdg` = heading × 100 |
| each F9 C4 | `BATTERY_STATUS` (#147) | `voltages[0]` = supply voltage in mV; the rest unknown |
| each F9 C0 | `ATTITUDE` (#30) | `yaw` = heading in radians, wrapped to (−π, π]; roll and pitch 0. A yaw below 1e‑6 rad is sent as 1e‑6: KoggerApp drops an ATTITUDE whose three angles are all zero |
| C0 bytes 2–11 changed, or the satellites crossed 4 | `STATUSTEXT` (#253, DEBUG) | `F9 C0 front 00 01 00 00 00 00 00 00 00 00 sats 38`: to find a fix flag in a capture |
| once a second while frames come, until 5 s after the last | `HEARTBEAT` (#0) | surface boat, autopilot GENERIC, active |

### Valid position

A valid position meets three conditions:
- both coordinates are in range;
- neither coordinate is exactly 0;
- at least 4 satellites.

Why each rule:
- The controller keeps sending its last or a factory position when it has no fix. This was seen with 1 satellite and a
  frozen position about 1000 km away.
- Once it sent −0.3742232 / 0.0 for about a second between real fixes.
- The threshold of 4 satellites is an inference: there is no 3D fix with fewer. What byte 17 counts exactly is still to
  be checked on water.
- A frozen position with 4 or more satellites is not caught: C4 has no fix flag, and the front of C0 is not decoded yet.

### Other details

- The heading reaches `ATTITUDE` from C0 only, not from C4. With C0 at 8 Hz a sonar mosaic gets a heading at most
  0.125 s old.
- **Autopilot GENERIC, not INVALID.** KoggerApp treats a HEARTBEAT with autopilot INVALID as no vehicle: no map marker,
  no GPS panel. The price: a ground station may take the module for an autopilot and ask for parameters, missions or a
  home position. No answer comes, because the line only listens.
- The raw F9 bytes are not relayed.

### Traffic and resolution

- **Traffic:** about 470 B/s with C4 at 1 Hz and C0 at 8 Hz, 12 frames/s.
- **Limit of the protocol:** the coordinates are float32 degrees. At 42.5° N / 124° E one step is 0.42 m in latitude and
  0.63 m in longitude, so a slow boat repeats its coordinates. One capture had 1.6 new values a second at 4.85 messages
  a second.

## In the ground station

- **Separate links.** Give line 1 its own UDP link (the module's port 14445), apart from the sonar line 0. KoggerApp
  may remove the devices of a link that carries a long run of MAVLink frames without a sonar frame. This is an
  inference from its code, not tested.
- **One position source.** Do not feed a second position source (NMEA, a phone) at the same time: the two would fight.

## Tests

`python tests/test_f9.py` checks four things:
- **Python rule.** The frame rule of `host/f9.py` matches the reference on random streams cut at random points.
- **C against Python.** The C (`f9.c`, `f9mav.c`, `mavcrc.c`, compiled on the PC) matches `host/f9.py` byte for byte:
  - frames and counters;
  - MAVLink from 210 C4 and 209 C0 frames, including odd values (negative, beyond 180, 360, NaN, infinity, no
    position);
  - the sequence wrap, `HEARTBEAT`, `STATUSTEXT`.
- **Relay.** The relay's framer takes every frame whole.
- **Independent decode.** With pymavlink installed, all five messages are decoded independently.

## Verified on hardware

The translation code has been the same since 2026‑10‑08. All of the following ran on builds made before it joined this
repository.
- **2026‑10‑07, KoggerApp.** A module on the telemetry wire of a GODIO controller; KoggerApp showed the boat's position
  and heading (the heading then still came from C4). With the heading from C0, line 1 put out 12 frames/s
  (8 `ATTITUDE`, 3 from C4, 1 `HEARTBEAT`), 471 B/s, nothing dropped.
- **2026‑10‑07, field capture (87 s).** After the controller update C4 and C0 came at 4.78 Hz each, and the MAVLink was
  as designed. The capture showed the frozen‑position case that the satellite threshold now covers.
- **2026‑10‑08, indoor capture (26–29 satellites).** The 55 `GPS_RAW_INT` with fewer than 4 satellites carried no
  position; 377 of 432 positions were sent.
- **2026‑10‑08, the current code:** 15.4 frames/s indoors, without a fix and so without `GLOBAL_POSITION_INT`.
- **2026‑11‑05, firmware 0.21.** A module set up as in the profile above reported the translator saved and running after
  its reboot. No controller was on its wire.
