#!/usr/bin/env python3
"""
Bench setup for the F9 -> MAVLink test: a module whose firmware carries the GODIO F9 translator (every build
from 0.23, docs/GODIO_F9.md) goes into the access-point role, with line 1 (X2) listening on the boat's telemetry
wire, translating it and broadcasting the MAVLink it makes to the access point's subnet. For a boat the module
profile profiles/boat_godio.json (tools/profile.py) sets all of this at once.

    python set_godio_ap.py --port COM3                 # defaults: RX GPIO5 (X2 pin 1), UDP 14445
    python set_godio_ap.py --port COM3 --rx-pin 4      # if the telemetry wire is on X2 pin 2

Line 1: RX = --rx-pin, TX parked on GPIO10 (not on any header; the translator never transmits), 9600 baud,
input translator "telemetry", UDP broadcast to --udp-port (and listening on it). Role: access point with the
module's saved access point settings (factory network unless changed), then reboot - the translator starts with
that boot. Everything is read back after the reboot.
Talks Kogger SBP (ID_WIFI_NET) over the module's X1 port with tools/bench_sbp.py's Dev class.
"""
import argparse
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # the ESP32_WiFi tree
sys.path.insert(0, os.path.join(ROOT, 'host'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import sbpframe as SB  # noqa: E402
import bench_sbp as B  # noqa: E402

TX_PARK = 10   # GPIO10: allowed for line 1 (netcfg_pin_ok), not brought out on the reference board
OK = 1         # SBP_RESP_OK (sbp.h); 0 is not a response code


def resp(code):
    return SB.RESP.get(code, 'code %s' % code)


def find(d):
    for r in (0, 87, 88):
        d.route = r
        if d.get(SB.ID_WIFI_NET, 0, timeout=1.0) is not None:
            return r
    return None


def line1(d):
    got = B.get_all(d, SB.ID_WIFI_NET, 3, bytes([1]))
    return SB.parse_line(got[0].payload) if got else None


def main():
    ap = argparse.ArgumentParser(description='access point + line 1 on the telemetry wire, for the F9 test')
    ap.add_argument('--port', required=True)
    ap.add_argument('--baud', type=int, default=921600, help='X1 rate of the module firmware')
    ap.add_argument('--rx-pin', type=int, default=5, help='GPIO of the telemetry wire: 5 = X2 pin 1, 4 = X2 pin 2')
    ap.add_argument('--udp-port', type=int, default=14445)
    a = ap.parse_args()

    d = B.Dev(a.port, a.baud)
    r = find(d)
    if r is None:
        sys.exit('no module answers ID_WIFI_NET on address 0/87/88 at %d baud' % a.baud)
    print('module answers on address %d' % r)

    cur = line1(d)
    if cur is None:
        sys.exit('no line 1 record (ID_WIFI_NET v3)')
    print('line 1 before :', cur)
    pl = SB.line_payload_from(cur, mode='udp', dest='broadcast', ip='0.0.0.0', rport=a.udp_port,
                              lport=a.udp_port, baud=9600, tx_pin=TX_PARK, rx_pin=a.rx_pin, xlat='telemetry')
    code, echo = d.setting(SB.ID_WIFI_NET, 3, pl, timeout=2.0)
    print('set line 1    : %s, checksum echo %s' % (resp(code), echo))
    if code != OK:
        sys.exit('line 1 refused (%s): nothing else changed' % resp(code))
    print('line 1 now    :', line1(d))

    role = SB.parse_role(d.get(SB.ID_WIFI_NET, 0).payload)
    print('role before   :', role)
    code, echo = d.setting(SB.ID_WIFI_NET, 0, SB.role_payload('ap', reboot=True), timeout=2.0)
    print('set role ap   : %s, checksum echo %s (module reboots)' % (resp(code), echo))
    if code != OK:
        sys.exit('role refused (%s)' % resp(code))

    time.sleep(1.5)
    B.wait_back(d, [88, r, 87, 0], timeout=25.0)
    f = d.get(SB.ID_WIFI_NET, 0, timeout=2.0)
    if f is None:
        sys.exit('module did not come back after the reboot (power-cycle it and run again)')
    role = SB.parse_role(f.payload)
    apc = d.get(SB.ID_WIFI_NET, 1, timeout=2.0)
    ipc = d.get(SB.ID_WIFI_NET, 2, timeout=2.0)
    print('after reboot  : address %d, role %s' % (d.route, role))
    if apc is not None:
        print('access point  :', SB.parse_ap(apc.payload))
    if ipc is not None:
        print('address/DHCP  :', SB.parse_ipcfg(ipc.payload))
    after = line1(d)
    print('line 1 after  :', after)
    xlat = (after or {}).get('xlat_running')
    ok = role.get('running') == 'ap' and xlat == 'telemetry'
    print('RESULT: %s' % ('access point up, line 1 translating; join its network and listen on UDP %d' % a.udp_port
                          if ok else 'role %s, line 1 translator %s' % (role.get('running'), xlat)))
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
