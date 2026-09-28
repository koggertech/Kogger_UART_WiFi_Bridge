#!/usr/bin/env python3
"""Bench check of the 0.12 port rules (docs/SBP_WIFI.md) over one serial port of the module (X1 or X2).

  python tools/bench_ports.py --port COM3                 # discovery, port information, rates, addresses
  python tools/bench_ports.py --port COM3 --baud 115200   # the port runs at another rate (X2 of a module set up before 0.13)
  python tools/bench_ports.py --port COM3 --long          # + reports stop 60 s after the last request

The module is found with discovery (GETTING ID_VERSION to route 0), as KoggerApp finds it, so its own
address need not be known. What it checks:
  - discovery to 0 and 255 is answered from the own address, other frames to 0 are not taken;
  - ID_WIFI_NET v7: page 0 of both ports ("asked here" on this port, host on this port), pages 1 and 2;
  - the unsolicited v1 report reaches this port after a request;
  - a rate change of this port (ID_UART v0) is provisional: confirmed by a request at the new rate (saved),
    or back to the old rate after 10 s without one (not saved); ID_WIFI_NET v3 keeps this port's rate
    (a written-back record must not move the port the host sits on);
  - a rate change of the other port is saved at once (and put back);
  - addresses 0 and 255 are refused (ID_UART v1/v2, ID_WIFI_NET v6).
Every changed setting is put back. Exit code 0 = all checks PASS.
"""
import argparse
import os
import struct
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'host'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import sbpframe as SB  # noqa: E402
from bench_sbp import Dev, KEY, check, get_all, results  # noqa: E402

V_LINE, V_PORTS, V_ADDR = 3, 7, 6


def discover(d, route=0):
    """Discovery like KoggerApp: GETTING ID_VERSION v2 to route 0/255. Returns (own route, payload)."""
    d.rx.clear()
    d.p.write(SB.encode(route, SB.mode(SB.GETTING, 2, resp=True), SB.ID_VERSION, b''))
    f = d.wait(lambda f: f.id == SB.ID_VERSION and f.ver == 2 and f.type == SB.CONTENT and not f.resp, 1.5)
    return (f.route, f.payload) if f else (None, None)


def pages(d, payload=b'', timeout=1.0):
    return [SB.parse_port(f.payload) for f in get_all(d, SB.ID_WIFI_NET, V_PORTS, payload, timeout)]


def page0(d, port):
    p = pages(d, bytes([port]))
    return p[0] if p else None


def uart_rate(d):
    f = d.get(SB.ID_UART, 0, KEY + bytes([1]))
    return struct.unpack('<I', f.payload[5:9])[0] if f and len(f.payload) >= 9 else None


def set_rate_uart(d, baud):
    return d.setting(SB.ID_UART, 0, KEY + bytes([1]) + struct.pack('<I', baud))


def line_rec(d, line):
    f = get_all(d, SB.ID_WIFI_NET, V_LINE, bytes([line]))
    return SB.parse_line(f[0].payload) if f else None


def set_line_baud(d, rec, baud):
    return d.setting(SB.ID_WIFI_NET, V_LINE, SB.line_payload_from(rec, baud=baud))  # carries the key


def provisional_test(d, port, old, new, how):
    """Change this port's rate, confirm it at the new rate, then change it back the same way."""
    code, _ = set_rate_uart(d, new) if how == 'uart' else set_line_baud(d, line_rec(d, port), new)
    check('%s: rate %d on this port -> OK (acknowledged at the old rate)' % (how, new), code == 1, code)
    time.sleep(0.3)
    d.baud(new)
    p = page0(d, port)
    check('%s: the module answers at %d; the request confirmed it: saved %d, not provisional' % (how, new, new),
          p is not None and p['baud'] == new and p['saved'] == new and not p['flags']['provisional'],
          p and (p['baud'], p['saved'], p['flags']['provisional']))
    code, _ = set_rate_uart(d, old) if how == 'uart' else set_line_baud(d, line_rec(d, port), old)
    time.sleep(0.3)
    d.baud(old)
    p = page0(d, port)
    check('%s: back to %d and confirmed (saved %d)' % (how, old, old),
          code == 1 and p is not None and p['baud'] == old and p['saved'] == old, p and (p['baud'], p['saved']))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', required=True)
    ap.add_argument('--baud', type=int, default=921600)
    ap.add_argument('--try-rate', type=int, default=460800, help='rate for the provisional change tests')
    ap.add_argument('--long', action='store_true', help='also wait 61 s: reports stop without requests')
    a = ap.parse_args()
    d = Dev(a.port, a.baud)
    d.pump(0.3)

    # ---- discovery
    route, v2 = discover(d, 0)
    if not check('discovery to route 0 answered from the own address (board 87)',
                 route not in (None, 0, 255) and v2 is not None and len(v2) >= 9 and v2[2] == 87,
                 (route, v2 and v2.hex())):
        sys.exit(1)
    d.route = route
    fw = (v2[8], v2[7])
    check('firmware 0.12 or later (%d.%d)' % fw, fw >= (0, 12), fw)
    r255, _ = discover(d, 255)
    check('discovery to route 255 answered from the same address', r255 == route, r255)
    d.rx.clear()
    d.p.write(SB.encode(0, SB.mode(SB.GETTING, 0, resp=True), SB.ID_WIFI, b''))
    f = d.wait(lambda f: f.id == SB.ID_WIFI and f.ver == 0, 1.0)  # not the periodic v1 report
    check('other frames to route 0 are not taken by the module (GETTING ID_WIFI to 0: no answer)', f is None, f)

    # ---- port information
    ps = pages(d)
    by = {p['port']: p for p in ps if p['page'] == 0}
    check('v7 without payload: page 0 of both ports', sorted(by) == [0, 1], sorted(by))
    here = [p for p in by.values() if p['flags']['asked_here']]
    ok = len(here) == 1
    check('exactly one port is marked "asked here"', ok, [(p['port'], p['flags']) for p in by.values()])
    if not ok:
        sys.exit(1)
    me = here[0]['port']
    other = 1 - me
    p = by[me]
    check('this port (%s): UART runs, rate %d, host seen, gets reports, not provisional' % (SB.PORT_NAMES[me], a.baud),
          p['flags']['uart'] and p['baud'] == a.baud and p['connected'] in ('sbp_host', 'sbp_host_devices') and
          p['flags']['reports'] and not p['flags']['provisional'] and p['module_rx'] > 0 and p['module_tx'] > 0,
          p)
    p1 = pages(d, bytes([me, 1]))
    p2 = pages(d, bytes([me, 2]))
    check('v7 pages 1 and 2 of this port parse', len(p1) == 1 and p1[0]['page'] == 1 and len(p2) == 1 and
          p2[0]['page'] == 2, (p1, p2))
    code, _ = d.setting(SB.ID_WIFI_NET, V_PORTS, KEY)
    check('v7 SETTING -> ERR_TYPE (read-only)', code == 6, code)
    a0 = get_all(d, SB.ID_WIFI_NET, V_PORTS, bytes([2]))
    d.rx.clear()
    d.send(SB.GETTING, V_PORTS, SB.ID_WIFI_NET, bytes([0, 9]))
    ack = d.wait(lambda f: f.id == SB.ID_WIFI_NET and f.ver == V_PORTS and f.resp, 1.0)
    check('v7 port 2 / page 9 -> ERR_PAYLOAD', a0 == [] and ack is not None and ack.payload[0] == 3,
          ack and ack.payload.hex())

    # ---- reports
    d.rx.clear()
    d.get(SB.ID_WIFI, 0)
    rep = d.collect(lambda f: f.id == SB.ID_WIFI and f.ver == 1 and f.type == SB.CONTENT, 2.5)
    check('the periodic v1 report reaches this port after a request (%d in 2.5 s)' % len(rep), len(rep) >= 1)

    # ---- rates of this port: confirmed, and back without confirmation
    check('ID_UART GET v0 = this port\'s rate', uart_rate(d) == a.baud, uart_rate(d))
    provisional_test(d, me, a.baud, a.try_rate, 'uart')
    code, _ = set_line_baud(d, line_rec(d, me), a.try_rate)
    p = page0(d, me)
    check('v3 with rate %d on this port\'s own line: OK, rate kept at %d' % (a.try_rate, a.baud),
          code == 1 and p is not None and p['baud'] == a.baud and not p['flags']['provisional'],
          p and (code, p['baud']))
    code, _ = set_rate_uart(d, a.try_rate)
    check('unconfirmed change: rate %d -> OK, the host stays at %d' % (a.try_rate, a.baud), code == 1, code)
    time.sleep(3)
    d.rx.clear()
    d.pump(8.5)
    p = page0(d, me)
    check('after 10 s without a request at %d the port is back at %d, saved rate unchanged' % (a.try_rate, a.baud),
          p is not None and p['baud'] == a.baud and p['saved'] == a.baud and not p['flags']['provisional'],
          p and (p['baud'], p['saved']))

    # ---- rate of the other port: saved at once
    rec = line_rec(d, other)
    po = page0(d, other)
    if rec and po and po['flags']['uart']:
        old = po['baud']
        new = 57600 if old != 57600 else 38400
        code, _ = set_line_baud(d, rec, new)
        p = page0(d, other)
        check('other port (%s): rate %d saved at once, not provisional' % (SB.PORT_NAMES[other], new),
              code == 1 and p is not None and p['baud'] == new and p['saved'] == new and not p['flags']['provisional'],
              p and (code, p['baud'], p['saved']))
        code, _ = set_line_baud(d, line_rec(d, other), old)
        p = page0(d, other)
        check('other port back at %d (saved)' % old, code == 1 and p is not None and p['baud'] == old and
              p['saved'] == old, p and (p['baud'], p['saved']))
    else:
        check('other port: UART off, rate test skipped', True, po)

    # ---- addresses
    for ver, pl in ((1, KEY + bytes([1, 0])), (1, KEY + bytes([1, 255])), (2, KEY + bytes([0])), (2, KEY + bytes([255]))):
        code, _ = d.setting(SB.ID_UART, ver, pl)
        check('ID_UART v%d address %d refused (ERR_PAYLOAD)' % (ver, pl[-1]), code == 3, code)
    for addr in (0, 255):
        code, _ = d.setting(SB.ID_WIFI_NET, V_ADDR, KEY + bytes([addr]))
        check('ID_WIFI_NET v6 address %d refused (ERR_PAYLOAD)' % addr, code == 3, code)
    f = d.get(SB.ID_UART, 1, KEY + bytes([1]))
    check('ID_UART v1 = the own address %d' % route, f is not None and f.payload[5] == route, f and f.payload.hex())

    # ---- a burst of requests, as KoggerApp sends when it opens a port: every one answered (0.14)
    kinds = [(SB.ID_WIFI_NET, 0), (SB.ID_WIFI_NET, 1), (SB.ID_WIFI_NET, 2), (SB.ID_WIFI_NET, V_ADDR)]
    f = d.get(SB.ID_WIFI, 1)
    drops0 = SB.parse_link(f.payload).get('req_drops') if f else None
    d.rx.clear()
    d.p.write(b''.join(SB.encode(route, SB.mode(SB.GETTING, kinds[i % 4][1]), kinds[i % 4][0], b'') for i in range(60)))
    got = d.collect(lambda f: (f.id, f.ver) in kinds and f.type == SB.CONTENT and not f.resp, 4)
    check('a burst of 60 requests: every one answered', len(got) == 60, '%d answers' % len(got))
    f = d.get(SB.ID_WIFI, 1)
    drops1 = SB.parse_link(f.payload).get('req_drops') if f else None
    check('ID_WIFI v1: requests dropped for a full queue, none in the burst', drops0 is not None and drops1 == drops0,
          '%s -> %s' % (drops0, drops1))

    if a.long:
        d.rx.clear()
        d.pump(61)
        d.rx.clear()
        rep = d.collect(lambda f: f.id == SB.ID_WIFI and f.ver == 1, 3)
        check('61 s without a request: no more reports on this port', rep == [], len(rep))

    failed = [n for n, ok in results if not ok]
    print('%d checks, %d failed' % (len(results), len(failed)))
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
