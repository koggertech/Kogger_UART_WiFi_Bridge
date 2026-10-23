#!/usr/bin/env python3
"""Bench checks for the radio switch-off (ID_WIFI_RF 0x5A, firmware 0.20, docs/SBP_WIFI.md §4b).

  python tools/bench_rf.py --port COM3                          # the module on this port, over its wire
  python tools/bench_rf.py --port /dev/ttyUSB0 --route 88 --secs 20   # the access point behind it, over the air

Over the wire the module is switched off without a timer, checked (state, refusals), given a timer while off and
left to come back by itself; then off again and back on by request. Over the air (--route) a switch-off without a
timer must be refused, and one with --secs is watched from the module on the port: it loses the access point, its
scan no longer finds it, and after the timer it joins again - the far module did not reboot (its uptime grew).
Every setting is left as it was: nothing of a switch-off is saved. Exit code 0 = all checks PASS.
"""
import argparse
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'host'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import sbpframe as SB  # noqa: E402
from bench_sbp import Dev, check, get_all, results  # noqa: E402
from bench_linkrate import find_at  # noqa: E402
from module_state import find_module  # noqa: E402

ERR_PAYLOAD, ERR_VERSION, ERR_KEY, ERR_RUNTIME = 3, 5, 7, 8


def at(d, route, fn):
    """Run fn() with requests going to `route`."""
    keep, d.route = d.route, route
    try:
        return fn()
    finally:
        d.route = keep


def content(d, id_, ver, timeout, resp=False):
    """The next CONTENT of `id_`/`ver` from the module asked: the route byte carries the sender's address, so a frame of
    the other module on the port (a periodic report, a state notice) is not taken for its answer."""
    return d.wait(lambda f: f.id == id_ and f.ver == ver and f.type == SB.CONTENT and f.resp == resp
                  and f.route == d.route, timeout)


def ask(d, id_, ver, payload=b'', timeout=2.0):
    d.send(SB.GETTING, ver, id_, payload)
    return content(d, id_, ver, timeout)


def setting(d, id_, ver, payload, timeout=2.0):
    """Response code of a SETTING, or None."""
    d.send(SB.SETTING, ver, id_, payload)
    a = content(d, id_, ver, timeout, resp=True)
    return a.payload[0] if a and a.payload else None


def rf_state(d, timeout=2.0):
    f = ask(d, SB.ID_WIFI_RF, 0, timeout=timeout)
    return SB.parse_rf(f.payload) if f else None


def rf_set(d, on, secs=0, payload=None, timeout=2.0):
    """SETTING v0: (response code, the CONTENT v0 that follows it or None)."""
    d.rx.clear()
    code = setting(d, SB.ID_WIFI_RF, 0, SB.rf_payload(on, secs) if payload is None else payload, timeout)
    f = content(d, SB.ID_WIFI_RF, 0, timeout)
    return code, SB.parse_rf(f.payload) if f else None


def status(d, timeout=2.0):
    f = ask(d, SB.ID_WIFI, 0, timeout=timeout)
    return SB.parse_status(f.payload) if f else None


def uptime(d):
    f = ask(d, SB.ID_WIFI, 1, timeout=2.0)
    return SB.parse_link(f.payload).get('uptime') if f else None


def wait_for(pred, timeout, every=1.0):
    """Poll pred() until it returns a truthy value: (value, seconds taken) or (None, timeout)."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        v = pred()
        if v:
            return v, time.time() - t0
        time.sleep(every)
    return None, timeout


def wired(d, secs):
    st0 = status(d)
    if not check('the module reports its state', st0):
        return
    role = 'AP' if st0['state'] == 'AP' else 'station'
    want = st0['state']  # where it has to come back to
    print('module on the port: %s, role %s, autoconnect %s' % (want, role, st0['auto']))
    up0 = uptime(d)

    s = rf_state(d)
    check('GETTING v0: the radio runs, no timer', s == dict(on=True, left=0), s)
    code = setting(d, SB.ID_WIFI_RF, 1, SB.rf_payload(False, 0))
    check('SETTING v1 -> ERR_VERSION', code == ERR_VERSION, code)
    code, s = rf_set(d, False, payload=(0x12345678).to_bytes(4, 'little') + bytes([0, 0, 0]))
    check('a wrong key -> ERR_KEY, the radio still runs', code == ERR_KEY and s and s['on'], (code, s))
    code, s = rf_set(d, False, payload=SB.KEY + bytes([0, 0]))
    check('a short payload -> ERR_PAYLOAD', code == ERR_PAYLOAD and s and s['on'], (code, s))
    code, s = rf_set(d, False, payload=SB.KEY + bytes([2, 0, 0]))
    check('on = 2 -> ERR_PAYLOAD', code == ERR_PAYLOAD and s and s['on'], (code, s))

    code, s = rf_set(d, False, 0)
    check('off without a timer over the wire -> OK, CONTENT {off, 0}', code == 1 and s == dict(on=False, left=0),
          (code, s))
    time.sleep(0.6)
    st = status(d)
    check('ID_WIFI v0: IDLE with the switched-off flag (bit 3)', st and st['state'] == 'IDLE' and st['rf_off'], st)
    if role == 'AP':
        check('an access point that is off has no clients', st and st['clients'] == 0, st)
    code = setting(d, SB.ID_WIFI, 2, b'')
    check('a scan while off -> ERR_RUNTIME', code == ERR_RUNTIME, code)
    code = setting(d, SB.ID_WIFI_SURVEY, 0, SB.survey_payload(0, 1 << 5))
    check('a channel sweep while off -> ERR_RUNTIME', code == ERR_RUNTIME, code)
    d.rx.clear()
    d.send(SB.GETTING, 1, SB.ID_WIFI_SURVEY, SB.linkrate_payload(0))
    a = d.wait(lambda f: f.id == SB.ID_WIFI_SURVEY and f.ver == 1 and f.type == SB.CONTENT and f.route == d.route, 2.0)
    check('a link rate window while off -> ERR_RUNTIME', a is not None and a.resp and a.payload[:1] == bytes([ERR_RUNTIME]),
          a and (a.resp, a.payload.hex()))
    if role == 'station':
        code = setting(d, SB.ID_WIFI, 3, SB.connect_payload('bench-rf-none', '', save=False))
        check('joining a network while off -> ERR_RUNTIME', code == ERR_RUNTIME, code)
    time.sleep(3)
    st = status(d)
    check('3 s later still IDLE and off (nothing started the radio)', st and st['state'] == 'IDLE' and st['rf_off'], st)

    code, s = rf_set(d, False, secs)
    check('off again with a %d s timer while off -> OK, the timer set' % secs,
          code == 1 and s and not s['on'] and secs - 1 <= s['left'] <= secs, (code, s))
    t0 = time.time()
    s, took = wait_for(lambda: (lambda x: x if x and x['on'] else None)(rf_state(d, 1.0)), secs + 10)
    took = time.time() - t0
    check('the timer switched the radio back on after %.0f s (asked %d)' % (took, secs),
          s is not None and secs - 1.5 <= took <= secs + 3, '%.1f s' % took)
    st, took = wait_for(lambda: (lambda x: x if x and x['state'] == want and not x['rf_off'] else None)(status(d)), 45)
    check('then the state is %s again (%.0f s)' % (want, took), st is not None, st)

    code, s = rf_set(d, True)
    check('on while on -> OK, nothing changes', code == 1 and s == dict(on=True, left=0), (code, s))
    code, s = rf_set(d, False, 30)
    check('off for 30 s -> OK', code == 1 and s and not s['on'], (code, s))
    time.sleep(2)
    code, s = rf_set(d, True)
    check('on by request before the timer -> OK, on at once', code == 1 and s == dict(on=True, left=0), (code, s))
    st, took = wait_for(lambda: (lambda x: x if x and x['state'] == want and not x['rf_off'] else None)(status(d)), 45)
    check('and the state is %s again (%.0f s)' % (want, took), st is not None, st)
    up1 = uptime(d)
    check('the module did not reboot meanwhile (uptime %s -> %s s)' % (up0, up1),
          up0 is not None and up1 is not None and up1 > up0, (up0, up1))


def remote(d, route, near, secs):
    rst = at(d, route, lambda: status(d))
    if not check('the module at %d answers over the air: %s' % (route, rst and rst['state']), rst):
        return
    ap_mac = rst['bssid'] if rst['state'] == 'AP' else None
    ap_ssid = rst['ssid'] if ap_mac else None
    nst = at(d, near, lambda: status(d))
    print('near module %d: %s to %s' % (near, nst and nst['state'], nst and nst['ssid']))
    up0 = at(d, route, lambda: uptime(d))

    s = at(d, route, lambda: rf_state(d))
    check('GETTING v0 over the air: the radio runs', s == dict(on=True, left=0), s)
    code, s = at(d, route, lambda: rf_set(d, False, 0))
    check('off without a timer over the network -> ERR_PAYLOAD, still on', code == ERR_PAYLOAD and s and s['on'],
          (code, s))
    code, s = at(d, route, lambda: rf_set(d, False, SB.RF_NET_MAX_S + 1))
    check('off for %d s over the network -> ERR_PAYLOAD' % (SB.RF_NET_MAX_S + 1), code == ERR_PAYLOAD, (code, s))

    code, s = at(d, route, lambda: rf_set(d, False, secs, timeout=3.0))
    t0 = time.time()
    if not check('off for %d s over the network -> OK, CONTENT {off, %d} arrives before the radio stops' % (secs, secs),
                 code == 1 and s and not s['on'] and secs - 1 <= s['left'] <= secs, (code, s)):
        return
    lost, took = wait_for(lambda: (lambda x: x if x and x['state'] != 'CONNECTED' else None)(at(d, near, lambda: status(d))),
                          min(secs, 15), 0.5)
    check('the near module lost the access point (%s after %.0f s)' % (lost and lost['state'], took), lost, lost)
    if ap_mac:
        seen = None
        while time.time() - t0 < secs - 3:
            frames = [f for f in at(d, near, lambda: get_all(d, SB.ID_WIFI, 2, b'', timeout=4.0)) if f.route == near]
            nets = [SB.parse_net(f.payload) for f in frames if len(f.payload) >= 13]
            if nets or frames:
                seen = [n for n in nets if n.get('bssid') == ap_mac or n.get('ssid') == ap_ssid]
                break
            time.sleep(1)
        check('a scan of the near module while it is off does not find it (%s)' % ap_ssid, seen == [],
              'no scan answered' if seen is None else seen)
    s, _ = wait_for(lambda: (lambda x: x if x and x['state'] == 'CONNECTED' else None)(at(d, near, lambda: status(d))),
                    secs + 60 - (time.time() - t0))
    took = time.time() - t0
    check('the near module joined it again %.0f s after the switch-off (timer %d s)' % (took, secs),
          s is not None and took >= secs - 1, '%.0f s' % took)
    s = at(d, route, lambda: rf_state(d, 3.0))
    check('the far module reports its radio on, no timer', s == dict(on=True, left=0), s)
    up1 = at(d, route, lambda: uptime(d))
    check('the far module did not reboot (uptime %s -> %s s)' % (up0, up1),
          up0 is not None and up1 is not None and up1 > up0, (up0, up1))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', required=True)
    ap.add_argument('--baud', type=int, default=921600)
    ap.add_argument('--route', type=int, default=0, help='address of a module behind the one on the port (over the air)')
    ap.add_argument('--secs', type=int, default=0,
                    help='switch-off timer, s; 0 = 20 over the wire, 45 over the air (the near module has to reach '
                         'its search, where it can scan, while the far one is off)')
    a = ap.parse_args()

    d = Dev(a.port, a.baud)
    d.pump(0.3)
    near, v2 = find_module(d)
    if not check('the module on the port answers discovery', near is not None):
        sys.exit(1)
    d.route = near
    fw = (v2[8], v2[7])
    if a.route:
        route, v2r = find_at(d, a.route)
        if not check('the module at %d answers' % a.route, route is not None):
            sys.exit(1)
        fw = (v2r[8], v2r[7])
    check('firmware %d.%d can switch its radio off (0.20 or later)' % fw, fw >= (0, 20), '%d.%d' % fw)
    if fw >= (0, 20):
        if a.route:
            remote(d, a.route, near, a.secs or 45)
        else:
            wired(d, a.secs or 20)

    failed = [n for n, ok in results if not ok]
    print('%d checks, %d failed' % (len(results), len(failed)))
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
