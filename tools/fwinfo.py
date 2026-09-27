#!/usr/bin/env python3
"""Full firmware version, running OTA slot and its state, whatever protocol the link is locked to.

  python tools/fwinfo.py --port COM3

ID_VERSION over SBP only carries major.minor; the text VER over SLIP carries "fw=0.3.1 part=ota_1
ota=valid". If the link is locked to SBP, the module is rebooted first with ID_BOOT v0 (5 s window,
then a reboot) so the SLIP question can be the first frame after boot; afterwards the port is handed
back to SBP ("PROTO link=sbp"), so KoggerApp keeps working. Careful: rebooting an image that is still
pending confirmation rolls it back - that is the point of the check, not a side effect.

While a UART line bridges to the network (0.7+), the port carries only SBP from the very start: VER
cannot be asked at all and this tool returns nothing. Use ID_VERSION (sbp_update.py --sbp-only).
The reboot goes to the module's address: --route, or the address its ID_VERSION answer comes from
(0.12 answers ID_VERSION to address 0 from its own address).
"""
import argparse
import os
import sys
import time

import serial

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'host'))
import sbpframe as SB  # noqa: E402
import wbframe  # noqa: E402


def ask_ver(p, timeout):
    dec = wbframe.Decoder()
    p.reset_input_buffer()
    p.write(wbframe.encode(wbframe.T_CTL, b'7 VER'))
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        for t, pl in dec.feed(p.read(p.in_waiting or 1)):
            line = pl.decode('utf-8', 'replace')
            if t == wbframe.T_CTL and line.startswith('7 OK'):
                return dict(kv.split('=', 1) for kv in line.split()[2:] if '=' in kv)
    return None


def own_route(p, route):
    """The module's address: the route its ID_VERSION v2 answer (board 87) comes from, else `route`."""
    dec = SB.Decoder()
    p.reset_input_buffer()
    p.write(SB.encode(route, SB.mode(SB.GETTING, 2, resp=True), SB.ID_VERSION))
    end = time.monotonic() + 1.0
    while time.monotonic() < end:
        for f in dec.feed(p.read(p.in_waiting or 1)):
            if f.id == SB.ID_VERSION and f.ver == 2 and not f.resp and len(f.payload) == 9 and f.payload[2] == SB.BOARD_WIFI:
                return f.route
    return route


def info(port, baud, route=0):
    p = serial.Serial()
    p.port, p.baudrate, p.timeout = port, baud, 0.01
    p.dtr = p.rts = True
    p.open()
    v = ask_ver(p, 1.0)
    rebooted = False
    if v is None:  # locked to SBP: reboot through the update window, then ask first
        route = own_route(p, route)
        key = SB.KEY_CONFIRM.to_bytes(4, 'little')
        p.write(SB.encode(route, SB.mode(SB.SETTING, 0, resp=True), SB.ID_BOOT, key))
        time.sleep(6.5)
        rebooted = True
        end = time.monotonic() + 8
        while v is None and time.monotonic() < end:
            v = ask_ver(p, 0.5)
        if v is not None:  # the question locked the port to SLIP: hand it back to SBP
            p.write(wbframe.encode(wbframe.T_CTL, b'8 PROTO link=sbp'))
            time.sleep(0.3)
    p.close()
    return v, rebooted


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', required=True)
    ap.add_argument('--baud', type=int, default=921600)
    ap.add_argument('--route', type=int, default=0, help='module address to try first (0.11 with a bridging line: 87)')
    a = ap.parse_args()
    v, rebooted = info(a.port, a.baud, a.route)
    if v is None:
        sys.exit('no answer')
    print('fw=%s part=%s ota=%s reset=%s%s' % (v.get('fw'), v.get('part'), v.get('ota'), v.get('reset'),
                                               '  (rebooted to ask)' if rebooted else ''))
