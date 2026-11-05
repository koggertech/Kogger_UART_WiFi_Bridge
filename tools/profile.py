#!/usr/bin/env python3
"""Module profiles (0.22, ID_WIFI_PROFILE, docs/SBP_WIFI.md 4c) over a wire: which profile the module carries, load
one and prove every setting landed, or take the module's settings as a profile.

  python tools/profile.py --port COM3 status
  python tools/profile.py --port COM3 apply my_boat.json
  python tools/profile.py --port COM3 apply broken.json --expect line1   # refusal test: exit 0 if refused for that
  python tools/profile.py --port COM3 read > mine.json                   # passwords empty: the module never gives
                                                                          # them back; fill them in before a load
apply: upload in pieces, apply, then the reboot - the module comes back at the profile's rate of the port the tool
is on (followed here) and maybe another address (found again); then the profile's name, the "changed since" flag and
every setting are read back and compared (passwords cannot be). A rate above --max-baud on the tool's own port is
refused before anything is sent: the adapter could not follow it (CP2105: 2 Mbaud), the module would be lost to the
wire until the BOOT button resets its rates.
Exit: 0 done and proven, 1 refused or differs, 2 no module / no answer.
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
from bench_ports import page0  # noqa: E402
from bench_sbp import Dev  # noqa: E402
from module_state import find_module, read  # noqa: E402

P = SB.ID_WIFI_PROFILE


def status(d):
    f = d.get(P, 0, timeout=1.5)
    return SB.parse_profile_status(f.payload) if f else None


def own_port(d):
    """0 (X1) or 1 (X2): the port this request came through (ID_WIFI_NET v7 flag asked_here)."""
    for port in (0, 1):
        p = page0(d, port)
        if p and p['flags'].get('asked_here'):
            return port
    return None


def upload(d, blob):
    code, _ = d.setting(P, 1, SB.profile_begin_payload(blob))
    if code != 1:
        return 'begin: %s' % SB.RESP.get(code, code)
    for pl in SB.profile_chunk_payloads(blob):
        for _ in range(3):  # a piece sent again after a lost answer is taken as OK
            code, _ = d.setting(P, 2, pl)
            if code is not None:
                break
        if code != 1:
            return 'piece at %d: %s' % (int.from_bytes(pl[:2], 'little'), SB.RESP.get(code, code))
    return None


def settings(d):
    """module_state.read(), with X1's saved rate from the port page (ID_UART answers for the asking port only)."""
    now = read(d)
    if not now:
        return None
    p0 = page0(d, 0)
    now['settings']['x1_baud'] = p0['saved'] if p0 else None
    return now['settings']


def as_profile(st, s):
    """module_state.read() settings -> profile dict (passwords empty)."""
    keep = lambda d, *k: {x: d[x] for x in k}  # noqa: E731
    l1 = s['line1']
    return dict(
        name=st['name'] if st and st['loaded'] else 'Module %d' % s['address'], role=s['role'], address=s['address'],
        x1_baud=s['x1_baud'], radio=dict(s['radio']),
        ap=dict(keep(s['ap'], 'ssid'), password='', **keep(s['ap'], 'channel', 'auth', 'hidden', 'max_clients')),
        ip=dict(s['ipcfg']), line0=keep(s['line0'], 'mode', 'dest', 'ip', 'rport', 'lport'),
        line1=dict(keep(l1, 'mode', 'dest', 'ip', 'rport', 'lport', 'baud', 'tx_pin', 'rx_pin'), xlat=l1.get('xlat', 'none')),
        networks=[dict(ssid=n, password='') for n in s['saved_networks']])


def differences(pr, got):
    """Fields of the profile the module does not hold (passwords are not readable; an open access point keeps none)."""
    out = []

    def walk(a, b, path):
        if isinstance(a, dict):
            for k in a:
                if k != 'password':
                    walk(a[k], b.get(k) if isinstance(b, dict) else None, path + [k])
        elif a != b:
            out.append('%s: profile %r, module %r' % ('.'.join(path), a, b))
    want = json.loads(json.dumps(pr))
    want['networks'] = [n['ssid'] for n in pr.get('networks', [])]
    have = dict(got, networks=[n['ssid'] for n in got['networks']])
    walk(want, have, [])
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', required=True)
    ap.add_argument('--baud', type=int, default=921600)
    ap.add_argument('--max-baud', type=int, default=2000000, help='fastest rate the adapter can follow')
    ap.add_argument('cmd', choices=['status', 'apply', 'read'])
    ap.add_argument('file', nargs='?')
    ap.add_argument('--expect', help='apply: the refusal expected (%s)' % ', '.join(SB.PROFILE_ERRORS[1:]))
    a = ap.parse_args()
    d = Dev(a.port, a.baud)
    d.pump(0.3)
    route, v2 = find_module(d)
    if route is None:
        print('no answer from the module (board 87) to discovery at %d baud' % a.baud)
        return 2
    d.route = route
    print('module at address %d, firmware %d.%d' % (route, v2[8], v2[7]), file=sys.stderr)
    st = status(d)
    if a.cmd == 'status':
        if st is None:
            print('no answer to ID_WIFI_PROFILE (firmware before 0.22?)')
            return 2
        print(json.dumps(st, ensure_ascii=False))
        return 0
    if a.cmd == 'read':
        s = settings(d)
        if not s or any(v is None for v in s.values()):
            print('settings not read whole: %s' % s, file=sys.stderr)
            return 2
        print(json.dumps(as_profile(st, s), ensure_ascii=False, indent=2))
        return 0

    pr = json.load(open(a.file, encoding='utf-8'))
    blob = SB.profile_blob(pr)
    port = own_port(d)
    new_rate = pr['x1_baud'] if port == 0 else pr['line1']['baud'] if port == 1 else None
    print('profile "%s": %d bytes, crc %08x; this port X%s, its rate after the load %s' % (
        pr['name'], len(blob), SB.profile_crc(blob), '?' if port is None else port + 1, new_rate))
    if new_rate is None:
        print('cannot tell which port the tool is on: not loading')
        return 2
    if new_rate > a.max_baud and not a.expect:
        print('REFUSED HERE: the port would go to %d baud, the adapter follows %d at most' % (new_rate, a.max_baud))
        return 1
    err = upload(d, blob)
    if err:
        print('upload failed: %s' % err)
        return 1
    up = status(d)
    print('upload: %s of %s bytes held' % (up and up['received'], up and up['announced']))
    d.rx.clear()
    code, _ = d.setting(P, 3, SB.profile_apply_payload(), timeout=3.0)
    r = d.wait(lambda f: f.id == P and f.ver == 3 and f.type == SB.CONTENT and not f.resp, 1.0)
    reason = SB.parse_profile_reason(r.payload) if r else None
    print('apply: %s, reason %s' % (SB.RESP.get(code, code), reason))
    if a.expect:
        ok = code != 1 and reason == a.expect
        print('REFUSED AS EXPECTED' if ok else 'NOT REFUSED AS EXPECTED (%s)' % a.expect)
        if code == 1:
            print('it was SAVED: the module reboots into it')
        return 0 if ok else 1
    if code != 1:
        return 1
    t0 = time.monotonic()
    time.sleep(1.0)
    d.baud(new_rate)
    route, v2 = find_module(d, tries=30)
    if route is None:
        print('the module did not come back at %d baud' % new_rate)
        return 2
    d.route = route
    print('back after %.1f s at address %d, %d baud' % (time.monotonic() - t0, route, new_rate))
    st = status(d)
    s = settings(d)
    if st is None or not s or any(v is None for v in s.values()):
        print('NOT PROVEN: status %s, settings %s' % (st, s))
        return 2
    diff = differences(pr, as_profile(st, s))
    print('status: %s' % json.dumps(st, ensure_ascii=False))
    for x in diff:
        print('DIFFERS %s' % x)
    ok = st['loaded'] and st['name'] == pr['name'] and not st['modified'] and not diff
    print('PROFILE LOADED AND PROVEN' if ok else 'PROFILE NOT PROVEN')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
