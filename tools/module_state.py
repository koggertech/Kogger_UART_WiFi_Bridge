#!/usr/bin/env python3
"""Read the Wi-Fi module's saved settings over SBP as JSON, or check that they are what an earlier reading says.

  python tools/module_state.py --port COM3 --baud 921600 > before.json
  python tools/module_state.py --port COM3 --baud 921600 --compare before.json   # exit 1 if a setting differs

Settings (compared): role, access point (the password is never read back), address/DHCP, both line records, own
address, both port rates, radio, saved networks. Runtime (not compared): firmware version, Wi-Fi state and network,
link report. With --compare a module that was connected before is given --wait seconds to connect again, and the
network must be the same one. The module is found with discovery, so the address need not be known.
"""
import argparse
import json
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'host'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import sbpframe as SB  # noqa: E402
from bench_ports import line_rec, page0, uart_rate  # noqa: E402
from bench_sbp import Dev, get_all  # noqa: E402


def one(d, id_, ver, parse, payload=b''):
    f = d.get(id_, ver, payload)
    return parse(f.payload) if f else None


def find_module(d, tries=20):
    """Discovery answered by the Wi-Fi module (board 87): a relaying line also carries it to the devices behind the
    network, and their answers come back on the same port."""
    for _ in range(tries):
        d.rx.clear()
        d.p.write(SB.encode(0, SB.mode(SB.GETTING, 2, resp=True), SB.ID_VERSION, b''))
        f = d.wait(lambda f: f.id == SB.ID_VERSION and f.ver == 2 and f.type == SB.CONTENT and not f.resp
                   and len(f.payload) >= 9 and f.payload[2] == SB.BOARD_WIFI, 1.5)
        if f:
            return f.route, f.payload
        time.sleep(0.5)
    return None, None


def read(d, tries=20):
    route, v2 = find_module(d, tries)
    if route is None:
        return None
    d.route = route
    settings = dict(
        role=(one(d, SB.ID_WIFI_NET, 0, SB.parse_role) or {}).get('saved'),
        ap=one(d, SB.ID_WIFI_NET, 1, SB.parse_ap),
        ipcfg=one(d, SB.ID_WIFI_NET, 2, SB.parse_ipcfg),
        line0=line_rec(d, 0),
        line1=line_rec(d, 1),
        address=route,
        x1_baud=uart_rate(d),
        x2_baud=(page0(d, 1) or {}).get('baud'),
        radio=one(d, SB.ID_WIFI, 7, SB.parse_radio),
        saved_networks=[s['ssid'] for s in (SB.parse_saved(f.payload) for f in get_all(d, SB.ID_WIFI, 4, b''))
                        if 'ssid' in s])
    if settings['radio']:  # what the driver holds now is runtime
        settings['radio'] = {k: settings['radio'][k] for k in ('power_dbm', 'mode', 'bw', 'ps')}
    st = one(d, SB.ID_WIFI, 0, SB.parse_status)
    runtime = dict(firmware='%d.%d' % (v2[8], v2[7]), running_role=(one(d, SB.ID_WIFI_NET, 0, SB.parse_role) or {}).get('running'),
                   state=st['state'] if st else None, ssid=st['ssid'] if st else None, rssi=st['rssi'] if st else None,
                   link=one(d, SB.ID_WIFI, 1, SB.parse_link))
    return dict(settings=settings, runtime=runtime)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', required=True)
    ap.add_argument('--baud', type=int, default=921600)
    ap.add_argument('--compare', help='JSON of an earlier reading')
    ap.add_argument('--wait', type=int, default=60, help='seconds to wait for Wi-Fi to connect again (--compare)')
    a = ap.parse_args()
    d = Dev(a.port, a.baud)
    d.pump(0.3)
    now = read(d)
    if now is None:
        print('no answer from the module (board 87) to discovery at %d baud' % a.baud)
        return 1
    if not a.compare:
        print(json.dumps(now, indent=1))
        return 0
    before = json.load(open(a.compare, encoding='utf-8'))
    diff = [k for k in before['settings'] if before['settings'][k] != now['settings'].get(k)]
    for k in diff:
        print('CHANGED %s: %s -> %s' % (k, before['settings'][k], now['settings'].get(k)))
    was = before['runtime']
    if was.get('state') == 'CONNECTED':
        end = time.time() + a.wait
        while now['runtime']['state'] != 'CONNECTED' and time.time() < end:
            time.sleep(3)
            now = read(d) or now
        ok = now['runtime']['state'] == 'CONNECTED' and now['runtime']['ssid'] == was.get('ssid')
        print('Wi-Fi %s: %s %s (before: %s %s)' % ('KEPT' if ok else 'NOT BACK', now['runtime']['state'],
                                                 now['runtime']['ssid'], was.get('state'), was.get('ssid')))
        if not ok:
            diff.append('wifi')
    print('firmware %s -> %s' % (was.get('firmware'), now['runtime']['firmware']))
    print(json.dumps(now, indent=1))
    print('SETTINGS KEPT' if not diff else 'SETTINGS DIFFER: %s' % ', '.join(diff))
    return 1 if diff else 0


if __name__ == '__main__':
    sys.exit(main())
