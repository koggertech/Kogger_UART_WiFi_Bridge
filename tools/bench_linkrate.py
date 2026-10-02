#!/usr/bin/env python3
"""Bench checks for the link rate report (ID_WIFI_SURVEY GETTING v1, firmware 0.18, docs/SBP_WIFI.md §4a).

  python tools/bench_linkrate.py --port COM3                         # the module on this port
  python tools/bench_linkrate.py --port /dev/ttyUSB0 --baud 2000000 --route 88 --traffic  # a module behind it

The module sniffs its channel for a short window (200 ms by default) and reports the PHY rates of the frames its peer
sent: the access point for a station, every joined station for an access point. The window costs the link some
throughput while it runs (ESP-IDF: the sniffer has a great impact on it), which is why the module refuses a new one
sooner than 1 s after the last. No setting is changed. Exit code 0 = all checks PASS.
"""
import argparse
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'host'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import sbpframe as SB  # noqa: E402
from bench_sbp import Dev, check, results  # noqa: E402
from module_state import find_module  # noqa: E402

V = 1


def find_at(d, route, tries=6):
    """ID_VERSION v2 of the Wi-Fi module at `route` (discovery is answered by the nearest module only)."""
    for _ in range(tries):
        d.rx.clear()
        d.p.write(SB.encode(route, SB.mode(SB.GETTING, 2, resp=True), SB.ID_VERSION, b''))
        f = d.wait(lambda f: f.id == SB.ID_VERSION and f.ver == 2 and f.type == SB.CONTENT and not f.resp
                   and f.route == route and len(f.payload) >= 9 and f.payload[2] == SB.BOARD_WIFI, 1.5)
        if f:
            return route, f.payload
        time.sleep(0.3)
    return None, None


# Burst frames: CONTENT v7 of ID_WIFI_SURVEY (a version nothing uses) to address 200 (nobody): the near module relays
# them like any foreign frame, the far one passes them to its port, where no device takes them.
BURST = SB.encode(200, SB.mode(SB.CONTENT, 7), SB.ID_WIFI_SURVEY, bytes(250))


def measure(d, window, timeout, traffic=False, burst=0):
    """Pages of one window, or the response code when the module refused. With `traffic` small requests go to the
    module through the link while the window is open, so that its peer - this side's station - has frames to send
    (an idle link from the host side carries nothing an access point could time). With `burst` that many large
    frames go through the link at once, back to back: what the timing bound needs."""
    d.rx.clear()
    d.send(SB.GETTING, V, SB.ID_WIFI_SURVEY, SB.linkrate_payload(window))
    if burst:
        time.sleep(0.03)  # the request first: the window opens when it arrives
        d.p.write(BURST * burst)
    if traffic:
        until = time.time() + (window or SB.LINKRATE_WINDOW_DEF) / 1000.0
        while time.time() < until:
            d.p.write(SB.encode(d.route, SB.mode(SB.GETTING, 2, resp=True), SB.ID_VERSION, b''))
            time.sleep(0.01)
    pages, end = [], time.time() + timeout
    while time.time() < end:
        f = d.wait(lambda f: f.id == SB.ID_WIFI_SURVEY and f.ver == V and f.type == SB.CONTENT, end - time.time())
        if f is None:
            break
        if f.resp:  # an answer instead of pages: the module refused
            return None, f.payload[0] if f.payload else None
        pages.append(SB.parse_linkrate(f.payload))
        if len(pages) >= max(pages[0].get('total', 0), 1):
            break
    return pages, None


def show(pages):
    for p in pages:
        if 'peer' not in p:
            print('  no peer')
            continue
        print('  peer %s: %d frames in %d ms, RSSI %d dBm, negotiated %s' % (p['peer'], p['frames'], p['window_ms'],
                                                                            p['rssi'], p['phy']))
        for r in p['rates']:
            what = ('%.1f Mbit/s' % (r['kbps'] / 1000.0)) if r['kbps'] else 'rate not known'
            if r.get('by_timing'):
                what += ' (proven by frame timing)'
            extra = ' MCS%d' % r['code'] if r['kind'] == 'ht' else ' code 0x%02X' % r['code']
            bound = ('  timing bound %d kbit/s' % r['bound_kbps']) if r.get('bound_kbps') else ''
            print('    %-5s%s%s%s  %s  %d frames%s' % (r['kind'], extra, ' SGI' if r['sgi'] else '',
                                                    ' 40 MHz' if r['mhz'] == 40 else '', what, r['frames'], bound))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', required=True)
    ap.add_argument('--baud', type=int, default=921600)
    ap.add_argument('--route', type=int, default=0, help='address of the module to measure; 0 = the nearest')
    ap.add_argument('--window', type=int, default=0, help='ms, 0 = the module default (200)')
    ap.add_argument('--burst', type=int, default=0,
                    help='large frames to push through the link at once during the window (the timing bound needs '
                         'frames back to back; they go to address 200, which nobody answers)')
    ap.add_argument('--windows', type=int, default=1, help='measure this many windows and keep every page')
    ap.add_argument('--traffic', action='store_true',
                    help='send small requests through the link during the window (for an access point measured from '
                         'its station side, whose uplink is otherwise idle)')
    a = ap.parse_args()

    d = Dev(a.port, a.baud)
    d.pump(0.3)
    route, v2 = find_at(d, a.route) if a.route else find_module(d)
    if not check('the module answers (address %s)' % (a.route or 'by discovery'), route is not None):
        sys.exit(1)
    d.route = route
    fw = (v2[8], v2[7])
    check('firmware %d.%d has the link rate report (0.18 or later)' % fw, fw >= (0, 18), '%d.%d' % fw)
    st = d.get(SB.ID_WIFI, 0, timeout=2)
    status = SB.parse_status(st.payload) if st else {}
    radio = d.get(SB.ID_WIFI, 7, timeout=2)
    radio = SB.parse_radio(radio.payload) if radio else {}
    print('module %d: %s, radio %s, negotiated %s' % (route, status.get('state'), radio.get('mode'), radio.get('phy')))

    window = a.window or SB.LINKRATE_WINDOW_DEF
    time.sleep(1.1)  # a window may have run just before (the app polls): keep the gap
    t0 = time.time()
    pages, code = measure(d, a.window, window / 1000.0 + 3, traffic=a.traffic, burst=a.burst)
    took = time.time() - t0
    if not check('GETTING v1 answered with pages (%.2f s for a %d ms window)' % (took, window), pages, str(code)):
        sys.exit(1)
    show(pages)
    more = []
    for _ in range(a.windows - 1):
        time.sleep(1.1)
        extra, _ = measure(d, a.window, window / 1000.0 + 3, traffic=a.traffic, burst=a.burst)
        show(extra or [])
        more += extra or []
    peers = [p for p in pages if 'peer' in p]
    check('the answer came after the window, not before', took >= window / 1000.0 * 0.9, '%.2f s' % took)
    if status.get('state') == 'CONNECTED':
        check('a station has one peer: the access point it is joined to', len(peers) == 1 and
              peers[0]['peer'] == status.get('bssid'), str([p['peer'] for p in peers]) + ' vs ' + str(status.get('bssid')))
        check('the negotiated PHY mode in the page is the one the radio reports', peers and
              peers[0]['phy'] == radio.get('phy'), str(peers and peers[0]['phy']))
    elif status.get('state') == 'AP':
        check('an access point reports one page per joined station', len(peers) == status.get('clients', -1),
              '%d pages, %s clients' % (len(peers), status.get('clients')))
    heard = [p for p in peers if p['frames']]
    check('frames were heard from the peer (there is traffic on the link)', heard, 'none')
    all_rates = [r for p in pages + more if 'rates' in p for r in p['rates']]
    timed = [r for r in all_rates if r.get('bound_kbps') and r['kbps'] and not r.get('by_timing')]
    if timed:  # the method's control: a timing bound must never exceed a rate the 802.11 tables give
        check('the timing bound never exceeds a decoded rate (%d rates timed): the timestamps are sound' % len(timed),
              all(r['bound_kbps'] <= r['kbps'] * 1.05 for r in timed),
              str([(r['kind'], r['code'], r['kbps'], r['bound_kbps']) for r in timed]))
    for r in [r for r in all_rates if r['kind'] == 'lr']:
        print('  LR code 0x%02X: %s (timing bound %s kbit/s)' % (r['code'], '500 kbit/s, proven by frame timing'
              if r.get('by_timing') else 'not proven', r.get('bound_kbps')))
    for p in heard:
        check('every rate of %s outside LR is decoded to kbit/s' % p['peer'],
              all(r['kbps'] > 0 for r in p['rates'] if r['kind'] not in ('lr', 'other')), str(p['rates']))
        check('the listed rates hold most of the frames of %s' % p['peer'],
              sum(r['frames'] for r in p['rates']) >= 0.5 * p['frames'], str(p['rates']))

    _, code = measure(d, a.window, 1.5)
    check('a window sooner than 1 s after the last is refused (ERR_RUNTIME)', code == 8, str(code))
    time.sleep(1.1)
    again, code = measure(d, a.window, window / 1000.0 + 3)
    check('after the gap a new window is served', again and not code, str(code))
    code, _ = d.setting(SB.ID_WIFI_SURVEY, V, SB.linkrate_payload())
    check('SETTING v1 is refused (ERR_TYPE): the report is a read', code == 6, str(code))

    failed = [n for n, ok in results if not ok]
    print('%d checks, %d failed' % (len(results), len(failed)))
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
