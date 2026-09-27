#!/usr/bin/env python3
"""Bench check of the module as a Kogger SBP device (what an SBP host sees), over a serial port.

  python tools/bench_sbp.py --port COM3                     # identity, ID_WIFI, ID_WIFI_NET, reports, errors
  python tools/bench_sbp.py --port COM3 --bauds 115200,2000000   # + switch through these rates and back
  python tools/bench_sbp.py --port COM3 --role-test         # + reboot into the AP role and back (0.10+)
  python tools/bench_sbp.py --port COM3 --write-test        # + a saved change of line 1, restored (0.10+)

If the link was locked to SLIP (bench_flash.py ran first), a SLIP "PROTO link=sbp" frame switches it
(--sbp-only skips it: with a bridging line it would be relayed to the peer as raw bytes).
Rates are switched with ID_UART like KoggerApp does and are never saved (no ID_FLASH), so a power
cycle always brings the module back to its default rate. Radio settings changed by the test are put
back even when it is interrupted. Meant for the bench: it scans (the Wi-Fi link pauses for seconds)
and briefly changes settings, so do not run it on a module that carries live traffic.
Exit code 0 = all checks PASS.
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

results = []


def check(name, cond, detail=''):
    results.append((name, bool(cond)))
    print('%s  %s%s' % ('PASS' if cond else 'FAIL', name, ('  -- ' + str(detail)) if detail else ''), flush=True)
    return cond


class Dev:
    def __init__(self, port, baud, route=0):
        self.p = serial.Serial()
        self.p.port, self.p.baudrate, self.p.timeout = port, baud, 0.005
        self.p.dtr = self.p.rts = True
        self.p.open()
        self.route = route
        self.dec = SB.Decoder()
        self.rx = []

    def baud(self, b):
        self.p.baudrate = b
        self.dec = SB.Decoder()

    def pump(self, sec):
        end = time.monotonic() + sec
        while time.monotonic() < end:
            self.rx += self.dec.feed(self.p.read(self.p.in_waiting or 1))

    def send(self, ftype, ver, id_, payload=b''):
        f = SB.encode(self.route, SB.mode(ftype, ver, resp=ftype != SB.CONTENT), id_, payload)
        self.p.write(f)
        return f[-2], f[-1]

    def wait(self, pred, timeout=1.5):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            for i, f in enumerate(self.rx):
                if pred(f):
                    return self.rx.pop(i)
            self.pump(0.02)
        return None

    def collect(self, pred, timeout):
        self.pump(timeout)
        got = [f for f in self.rx if pred(f)]
        self.rx = [f for f in self.rx if not pred(f)]
        return got

    def get(self, id_, ver, payload=b'', timeout=1.5):
        self.send(SB.GETTING, ver, id_, payload)
        return self.wait(lambda f: f.id == id_ and f.ver == ver and f.type == SB.CONTENT and not f.resp, timeout)

    def setting(self, id_, ver, payload=b'', timeout=1.5):
        """Returns (response code or None, checksum echo matches)."""
        ck = self.send(SB.SETTING, ver, id_, payload)
        a = self.wait(lambda f: f.id == id_ and f.ver == ver and f.type == SB.CONTENT and f.resp, timeout)
        if a is None or not a.payload:
            return None, False
        return a.payload[0], len(a.payload) >= 3 and (a.payload[1], a.payload[2]) == ck


KEY = SB.KEY_CONFIRM.to_bytes(4, 'little')


def get_all(d, id_, ver, payload=b'', timeout=1.0):
    d.rx.clear()
    d.send(SB.GETTING, ver, id_, payload)
    return d.collect(lambda f: f.id == id_ and f.ver == ver and f.type == SB.CONTENT and not f.resp, timeout)


def wait_back(d, known, timeout=15.0):
    """After a reboot the module answers ID_VERSION again at the same rate. Its address may change: after a role
    change with reboot it keeps the previous one for one boot (0.11), later a bridging line makes it answer on its
    own address (87 station / 88 access point by default).
    Tries the addresses in `known` first; d.route ends on the one that answered, or stays unchanged."""
    end = time.monotonic() + timeout
    start = d.route
    routes = []
    for r in list(known) + [start, 0, 87, 88]:
        if r is not None and r not in routes:
            routes.append(r)
    while time.monotonic() < end:
        for r in routes:
            d.route = r
            if d.get(SB.ID_VERSION, 2, timeout=0.4):
                print('      module answers on address %d' % r)
                return True
    d.route = start
    return False


def net_checks(d, a, fw):
    """ID_WIFI_NET (0x58, firmware 0.10+). Nothing is saved unless --write-test / --role-test."""
    if fw < (0, 10):
        print('ID_WIFI_NET needs firmware 0.10+ (module runs %d.%d): skipped' % fw)
        return
    r = d.get(SB.ID_WIFI_NET, 0)
    if not check('ID_WIFI_NET v0 answers', r is not None and len(r.payload) >= 2):
        return
    role = SB.parse_role(r.payload)
    check('ID_WIFI_NET v0 role: %s' % role, role['saved'] in SB.ROLES and role['running'] in SB.ROLES)
    r = d.get(SB.ID_WIFI_NET, 1)
    apc = SB.parse_ap(r.payload) if r else None
    check('ID_WIFI_NET v1 access point (password not read back): %s' % apc, apc is not None and r.payload[-1] == 0)
    r = d.get(SB.ID_WIFI_NET, 2)
    ipc = SB.parse_ipcfg(r.payload) if r else None
    check('ID_WIFI_NET v2 address/DHCP: %s' % ipc, ipc is not None)
    lines = [SB.parse_line(f.payload) for f in get_all(d, SB.ID_WIFI_NET, 3)]
    check('ID_WIFI_NET v3 without payload: both lines', [x['line'] for x in lines] == [0, 1], lines)
    for x in lines:
        print('      line %(line)d: %(mode)s %(dest)s %(ip)s:%(rport)d lport %(lport)d, %(baud)d baud, '
              'pins %(tx_pin)d/%(rx_pin)d, uart %(uart)s' % x)
    l1 = [x for x in lines if x['line'] == 1]
    one = get_all(d, SB.ID_WIFI_NET, 3, bytes([1]))
    check('ID_WIFI_NET v3 {line 1}: that line only', len(one) == 1 and SB.parse_line(one[0].payload)['line'] == 1)
    st = [SB.parse_line_stats(f.payload) for f in get_all(d, SB.ID_WIFI_NET, 4)]
    check('ID_WIFI_NET v4 statistics of both lines', [x['line'] for x in st] == [0, 1],
          ['%s %s' % (x['line'], x['state']) for x in st])
    cl = [SB.parse_client(f.payload) for f in get_all(d, SB.ID_WIFI_NET, 5)]
    check('ID_WIFI_NET v5 clients: %s' % cl, cl and all(x['total'] == cl[0]['total'] for x in cl))
    r = d.get(SB.ID_WIFI_NET, 6)
    check('ID_WIFI_NET v6 own address while bridging: %s' % (r and r.payload[0]), r is not None and len(r.payload) >= 1)
    bridge_addr = r.payload[0] if r else None

    code, echo = d.setting(SB.ID_WIFI_NET, 3, bytes(4) + bytes(17))
    check('SETTING ID_WIFI_NET without key -> ERR_KEY', code == 7 and echo, (code, echo))
    code, _ = d.setting(SB.ID_WIFI_NET, 7, KEY)
    check('SETTING ID_WIFI_NET v7 -> ERR_TYPE (port information is read-only, 0.12; 0.11: ERR_VERSION)',
          code in (5, 6), code)
    code, _ = d.setting(SB.ID_WIFI_NET, 4, KEY)
    check('SETTING ID_WIFI_NET v4 (statistics) -> ERR_TYPE', code == 6, code)
    code, _ = d.setting(SB.ID_WIFI_NET, 2, SB.ipcfg_payload('10.0.0.10', '255.255.255.0', True, '10.0.1.11', '10.0.1.30'))
    check('SETTING v2 with the DHCP pool outside the subnet -> ERR_PAYLOAD', code == 3, code)
    code, _ = d.setting(SB.ID_WIFI_NET, 2, SB.ipcfg_payload('10.0.0.10', '255.255.255.0', True, '10.0.0.5', '10.0.0.20'))
    check('SETTING v2 with the own address inside the pool -> ERR_PAYLOAD', code == 3, code)
    code, _ = d.setting(SB.ID_WIFI_NET, 1, SB.ap_payload('x', 'short', auth='wpa2'))
    check('SETTING v1 with a 5-character WPA2 password -> ERR_PAYLOAD', code == 3, code)
    if l1:
        x = l1[0]
        code, _ = d.setting(SB.ID_WIFI_NET, 3, SB.line_payload_from(x, mode='udp', rport=0, lport=0))
        check('SETTING v3 UDP line without a port -> ERR_PAYLOAD', code == 3, code)
        code, _ = d.setting(SB.ID_WIFI_NET, 3, SB.line_payload_from(x, tx_pin=9))
        check('SETTING v3 line 1 on GPIO9 (boot strap) -> ERR_PAYLOAD', code == 3, code)
        code, _ = d.setting(SB.ID_WIFI_NET, 6, KEY + bytes([0]))
        check('SETTING v6 own address 0 -> ERR_PAYLOAD (0.11)', code == 3 or fw < (0, 11), code)
    if l1 and a.write_test:
        # Saves line 1: from now on its saved settings apply in both roles instead of the role default.
        x = l1[0]
        other = 57600 if x['baud'] != 57600 else 115200
        code, echo = d.setting(SB.ID_WIFI_NET, 3, SB.line_payload_from(x, baud=other))
        r = d.wait(lambda f: f.id == SB.ID_WIFI_NET and f.ver == 3 and f.type == SB.CONTENT and not f.resp)
        ok = code == 1 and echo and r is not None and SB.parse_line(r.payload)['baud'] == other
        check('SETTING v3 line 1 rate %d -> OK, read back' % other, ok, code)
        code, _ = d.setting(SB.ID_WIFI_NET, 3, SB.line_payload_from(x))
        r = d.wait(lambda f: f.id == SB.ID_WIFI_NET and f.ver == 3 and f.type == SB.CONTENT and not f.resp)
        check('line 1 restored', code == 1 and r is not None and SB.parse_line(r.payload) == x, r and SB.parse_line(r.payload))

    if not a.role_test:
        return
    back = role['running']
    home = d.route
    code, _ = d.setting(SB.ID_WIFI_NET, 0, SB.role_payload('ap', reboot=True))
    check('SETTING v0 role ap + reboot -> OK', code == 1, code)
    d.rx.clear()
    time.sleep(1.0)
    if not check('  module is back after the reboot', wait_back(d, [home, bridge_addr, 88])):
        print('  NOT FOUND: it may still run as access point. Find it (addresses 0/87/88, same rate) and send '
              'ID_WIFI_NET v0 {key, 0, 1} to return to the station role.')
        return
    r = d.get(SB.ID_WIFI_NET, 0)
    check('  running as access point', r is not None and SB.parse_role(r.payload)['running'] == 'ap',
          r and SB.parse_role(r.payload))
    st = None
    end = time.monotonic() + 5
    while time.monotonic() < end and not (st and st['state'] == 'AP'):
        f = d.get(SB.ID_WIFI, 0, timeout=0.5)
        st = SB.parse_status(f.payload) if f else None
    check('  ID_WIFI v0 state AP, own network %s' % (st and st['ssid']), st is not None and st['state'] == 'AP', st)
    code, _ = d.setting(SB.ID_WIFI, 3, SB.connect_payload('x', ''))
    check('  station connect in the AP role -> ERR_RUNTIME', code == 8, code)
    code, _ = d.setting(SB.ID_WIFI_NET, 0, SB.role_payload(back, reboot=True))
    check('SETTING v0 role %s + reboot -> OK' % back, code == 1, code)
    d.rx.clear()
    time.sleep(1.0)
    check('  module is back', wait_back(d, [home, bridge_addr]))
    r = d.get(SB.ID_WIFI_NET, 0)
    check('  running as %s again' % back, r is not None and SB.parse_role(r.payload)['running'] == back)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', required=True)
    ap.add_argument('--baud', type=int, default=921600, help='current module rate')
    ap.add_argument('--bauds', default='', help='comma-separated rates to switch through (then back)')
    ap.add_argument('--route', type=int, default=0)
    ap.add_argument('--radio-mode-test', action='store_true',
                    help='also switch to b/g and back (the module reassociates twice)')
    ap.add_argument('--role-test', action='store_true',
                    help='also reboot into the access point role and back (0.10+; the Wi-Fi link drops meanwhile)')
    ap.add_argument('--write-test', action='store_true',
                    help='also change line 1 for real and restore it (0.10+; saves line 1 in the module)')
    ap.add_argument('--sbp-only', action='store_true', help='do not send the SLIP "PROTO link=sbp" frame first')
    a = ap.parse_args()
    d = Dev(a.port, a.baud, a.route)

    if not a.sbp_only:
        d.p.write(wbframe.encode(wbframe.T_CTL, b'0 PROTO link=sbp'))   # no-op unless locked to SLIP
    d.pump(0.3)
    d.rx.clear()

    v0 = d.get(SB.ID_VERSION, 0)
    check('ID_VERSION v0: 34 bytes, board %d' % SB.BOARD_WIFI, v0 is not None and len(v0.payload) == 34
          and v0.payload[1] == SB.BOARD_WIFI, v0 and v0.payload[:4].hex())
    v2 = d.get(SB.ID_VERSION, 2)
    ok = v2 is not None and len(v2.payload) == 9 and v2.payload[2] == SB.BOARD_WIFI
    check('ID_VERSION v2: board %d, fw %s' % (SB.BOARD_WIFI, '%d.%d' % (v2.payload[8], v2.payload[7]) if ok else '?'), ok)
    if not ok:
        sys.exit('no module (or not board %d) at address %d, %d baud' % (SB.BOARD_WIFI, a.route, a.baud))
    fw = (v2.payload[8], v2.payload[7])  # what the module supports decides; a missing answer is then a FAIL
    v1 = d.get(SB.ID_VERSION, 1)
    check('ID_VERSION v1: 12-byte UID (MAC)', v1 is not None and len(v1.payload) == 12, v1 and v1.payload[:6].hex(':'))
    u = d.get(SB.ID_UART, 0, KEY + bytes([1]))
    ok = u is not None and len(u.payload) == 9 and u.payload[:4] == KEY
    check('ID_UART v0: key + uart 1 + current baud', ok, ok and int.from_bytes(u.payload[5:9], 'little'))

    st = d.get(SB.ID_WIFI, 0)
    s = None
    try:
        s = SB.parse_status(st.payload) if st else None
    except ValueError as e:
        check('ID_WIFI v0 parses', False, e)
    check('ID_WIFI v0 status', s is not None, s)

    reps = d.collect(lambda f: f.id == SB.ID_WIFI and f.ver == 1 and f.type == SB.CONTENT, 3.2)
    ok = len(reps) >= 2
    check('ID_WIFI v1 reports unsolicited, ~1 Hz (%d in 3.2 s)' % len(reps), ok,
          SB.parse_link(reps[-1].payload) if reps else '')

    code, echo = d.setting(SB.ID_WIFI, 1, (300).to_bytes(2, 'little'))
    check('SETTING ID_WIFI v1 period 300 ms -> OK, checksum echoed', code == 1 and echo, (code, echo))
    d.rx.clear()
    reps = d.collect(lambda f: f.id == SB.ID_WIFI and f.ver == 1 and f.type == SB.CONTENT and not f.resp, 2.0)
    check('  reports now ~3.3 Hz (%d in 2 s)' % len(reps), 5 <= len(reps) <= 8)
    code, _ = d.setting(SB.ID_WIFI, 1, (1000).to_bytes(2, 'little'))
    check('  period back to 1000 ms', code == 1)

    d.rx.clear()
    d.send(SB.GETTING, 2, SB.ID_WIFI)
    nets = d.collect(lambda f: f.id == SB.ID_WIFI and f.ver == 2 and f.type == SB.CONTENT and not f.resp, 6.0)
    parsed = []
    try:
        parsed = [SB.parse_net(f.payload) for f in nets]
    except ValueError as e:
        check('ID_WIFI v2 parses', False, e)
    total = parsed[0]['total'] if parsed else -1
    check('ID_WIFI v2 scan: %d frames, index 0..total-1' % len(parsed),
          parsed and sorted(p['index'] for p in parsed) == list(range(max(total, 1) if total else 1)) and
          all(p['total'] == total for p in parsed))
    for p in parsed[:12]:
        if 'ssid' in p:
            print('      %-32s rssi=%-4d ch=%-3d %-10s%s%s' % (p['ssid'], p['rssi'], p['ch'], p['auth'],
                                                             ' saved' if p['saved'] else '', ' CURRENT' if p['current'] else ''))

    sv = d.collect(lambda f: False, 0)  # drain
    d.send(SB.GETTING, 4, SB.ID_WIFI)
    saved = d.collect(lambda f: f.id == SB.ID_WIFI and f.ver == 4 and f.type == SB.CONTENT and not f.resp, 1.0)
    sp = [SB.parse_saved(f.payload) for f in saved]
    check('ID_WIFI v4 saved list: %s' % [x.get('ssid') for x in sp], sp and all(x['total'] == sp[0]['total'] for x in sp))

    code, echo = d.setting(SB.ID_WIFI, 3, bytes([1, 0, 0]))
    check('SETTING ID_WIFI v3 with empty SSID -> ERR_PAYLOAD', code == 3 and echo, (code, echo))
    code, _ = d.setting(SB.ID_UART, 0, bytes(4) + bytes([1]) + (115200).to_bytes(4, 'little'))
    check('ID_UART without key -> ERR_KEY', code == 7, code)
    code, _ = d.setting(SB.ID_UART, 0, KEY + bytes([1]) + (5000000).to_bytes(4, 'little'))
    check('ID_UART 5 Mbaud (above 4 M) -> ERR_PAYLOAD, rate unchanged', code == 3, code)
    code, _ = d.setting(SB.ID_MARK, 0, KEY)
    m = d.wait(lambda f: f.id == SB.ID_MARK and f.type == SB.CONTENT and not f.resp)
    check('ID_MARK set -> OK, frames now carry the mark bit', code == 1 and m is not None and m.mark)

    # ---- radio (ID_WIFI v7, firmware >= 0.8): settings are restored at the end, also on an interruption
    r = d.get(SB.ID_WIFI, 7) if fw >= (0, 8) else None
    if fw < (0, 8):
        print('ID_WIFI v7 needs firmware 0.8+: skipped')
    elif check('ID_WIFI v7 answers', r is not None and len(r.payload) >= 8):
        before = bytes(r.payload[:4])
        rr = SB.parse_radio(r.payload)
        check('ID_WIFI v7 radio: %s' % rr, True)
        try:
            code, echo = d.setting(SB.ID_WIFI, 7, SB.radio_payload(15, rr['mode'], rr['bw'], rr['ps']))
            r2 = d.wait(lambda f: f.id == SB.ID_WIFI and f.ver == 7 and f.type == SB.CONTENT and not f.resp, 1.5)
            ok = code == 1 and echo and r2 is not None and r2.payload[0] == 60 and 0 < r2.payload[4] <= 60
            check('SETTING v7 power 15 dBm -> OK, in effect %s dBm' % (r2 and r2.payload[4] / 4), ok, code)
            code, _ = d.setting(SB.ID_WIFI, 7, SB.radio_payload(15, 'b', 40, 'none'))
            check('SETTING v7 40 MHz with 11b only -> ERR_PAYLOAD', code == 3, code)
            if a.radio_mode_test:
                code, _ = d.setting(SB.ID_WIFI, 7, SB.radio_payload(15, 'bg', 20, rr['ps']))
                st = None
                end = time.monotonic() + 25
                while time.monotonic() < end:
                    f = d.get(SB.ID_WIFI, 0, timeout=1.0)
                    st = SB.parse_status(f.payload) if f else None
                    if st and st['state'] == 'CONNECTED':
                        break
                    time.sleep(0.5)
                r3 = d.get(SB.ID_WIFI, 7)
                phy = SB.parse_radio(r3.payload)['phy'] if r3 else None
                check('mode b/g -> reassociated, negotiated %s' % phy,
                      code == 1 and st is not None and st['state'] == 'CONNECTED' and phy == '11g', (code, st and st['state']))
        finally:
            code, _ = d.setting(SB.ID_WIFI, 7, before)
            check('radio settings restored', code == 1, code)

    net_checks(d, a, fw)

    rates = [int(x) for x in a.bauds.split(',') if x.strip()]
    cur = a.baud
    for b in rates + ([a.baud] if rates else []):
        code, echo = d.setting(SB.ID_UART, 0, KEY + bytes([1]) + b.to_bytes(4, 'little'))
        if code != 1:
            check('ID_UART -> %d: ACK at %d' % (b, cur), False, code)
            break
        time.sleep(0.05)
        d.baud(b)
        v = d.get(SB.ID_VERSION, 2, timeout=1.0) or d.get(SB.ID_VERSION, 2, timeout=1.0)
        ok = v is not None and len(v.payload) == 9 and v.payload[2] == SB.BOARD_WIFI
        check('ID_UART -> %d: ACK at old rate, module answers at new rate' % b, ok)
        if not ok:
            print('lost the module at %d; trying to find it again ...' % b)
            for probe in [b, cur, a.baud]:
                d.baud(probe)
                if d.get(SB.ID_VERSION, 2, timeout=0.7):
                    print('  found at', probe)
                    break
            else:
                print('  not found: power-cycle the module (rates are not saved)')
            break
        cur = b

    print('host decoder: ok=%d check_errors=%d' % (d.dec.frames_ok, d.dec.check_errors))
    d.p.close()
    failed = [n for n, ok in results if not ok]
    print('\n%d checks, %d failed' % (len(results), len(failed)))
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
