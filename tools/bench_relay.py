#!/usr/bin/env python3
"""Bench check of the SBP relay (docs/RELAY.md): the PC plays the boat over UDP and KoggerApp over the
serial port, and compares what comes out on each side with what went in.

  python tools/bench_relay.py --port COM3 --pc-ip 192.168.1.10            # module and PC on one IP network
  python tools/bench_relay.py --port COM3 --pc-ip 192.168.1.10 --off      # switch the relay off afterwards

Needs: the module connected to a Wi-Fi network in which the PC is reachable at --pc-ip (UDP --udp-port
open in the PC firewall), firmware >= 0.6. The relay is configured with ID_WIFI v5 (mode UDP, own
address --addr, peer = the PC), then:
  up    serial -> UDP: KP1/KP2 frames (some > 512 B) + raw bytes; packets <= 512 B, whole frames,
        frames > 512 cut into 512-byte pieces, concatenation == what was sent
  down  UDP -> serial: frames, one of them split across two datagrams; serial output (minus the
        module's own frames) == what was sent, own frames only between relayed frames
  local ID_WIFI v0 to the module's address is answered and not relayed
Exit code 0 = all checks PASS.
"""
import argparse
import os
import random
import socket
import struct
import sys
import time

import serial

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'host'))
import kframe as KF  # noqa: E402
import sbpframe as SB  # noqa: E402
import wbframe  # noqa: E402

results = []


def check(name, cond, detail=''):
    results.append((name, bool(cond)))
    print('%s  %s%s' % ('PASS' if cond else 'FAIL', name, ('  -- ' + str(detail)) if detail else ''), flush=True)
    return cond


class Port:
    def __init__(self, port, baud):
        self.p = serial.Serial()
        self.p.port, self.p.baudrate, self.p.timeout = port, baud, 0.002
        self.p.dtr = self.p.rts = True
        self.p.open()
        self.raw = bytearray()

    def pump(self, sec):
        end = time.monotonic() + sec
        while time.monotonic() < end:
            self.raw += self.p.read(self.p.in_waiting or 1)

    def frames(self):
        d = SB.Decoder()
        return d.feed(bytes(self.raw))

    def ask(self, route, ftype, ver, id_, payload=b'', timeout=1.5):
        self.raw.clear()
        self.p.write(SB.encode(route, SB.mode(ftype, ver, resp=ftype != SB.CONTENT), id_, payload))
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.pump(0.02)
            for f in self.frames():
                if f.id == id_ and f.ver == ver and f.type == SB.CONTENT and f.route == route:
                    return f
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', required=True)
    ap.add_argument('--baud', type=int, default=921600)
    ap.add_argument('--pc-ip', required=True, help='address of this PC as the module sees it')
    ap.add_argument('--udp-port', type=int, default=15057)
    ap.add_argument('--addr', type=int, default=87, help='module SBP address while relaying')
    ap.add_argument('--off', action='store_true',
                    help='switch the relay off at the end (default: restore what was set before the test)')
    a = ap.parse_args()

    port = Port(a.port, a.baud)
    port.p.write(wbframe.encode(wbframe.T_CTL, b'0 PROTO link=sbp'))
    port.pump(0.3)
    route = None
    for r in (a.addr, 0):
        f = port.ask(r, SB.GETTING, 2, SB.ID_VERSION, timeout=1.0)
        if f and len(f.payload) == 9 and f.payload[2] == SB.BOARD_WIFI:
            route = r
            break
    if not check('module found (address %s)' % route, route is not None):
        sys.exit(1)
    f = port.ask(route, SB.GETTING, 5, SB.ID_WIFI, timeout=1.0)
    orig = bytes(f.payload[:10]) if f and len(f.payload) >= 10 else None
    if not check('relay settings before the test read (restored at the end)', orig is not None):
        sys.exit(1)
    try:
        run(a, port, route)
    finally:
        restore = (bytes([0]) + orig[1:]) if a.off else orig  # the address read back is the module's own
        port.raw.clear()
        port.p.write(SB.encode(a.addr, SB.mode(SB.SETTING, 5, resp=True), SB.ID_WIFI, restore))
        port.pump(0.5)
        print('relay settings %s: mode %d, address %d' % ('switched off' if a.off else 'restored', restore[0], restore[1]))
        port.p.close()
    failed = [n for n, ok in results if not ok]
    print('\n%d checks, %d failed' % (len(results), len(failed)))
    sys.exit(1 if failed else 0)


def run(a, port, route):

    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind(('0.0.0.0', a.udp_port))
    udp.settimeout(0.05)
    ip = bytes(int(x) for x in a.pc_ip.split('.'))
    cfg = bytes([1, a.addr]) + ip + struct.pack('<HH', a.udp_port, a.udp_port)
    ack = None
    port.raw.clear()
    port.p.write(SB.encode(route, SB.mode(SB.SETTING, 5, resp=True), SB.ID_WIFI, cfg))
    end = time.monotonic() + 2
    while time.monotonic() < end and ack is None:
        port.pump(0.02)
        for f in port.frames():
            if f.id == SB.ID_WIFI and f.ver == 5 and f.resp:
                ack = f
    check('ID_WIFI v5: relay UDP -> %s:%d, module address %d' % (a.pc_ip, a.udp_port, a.addr),
          ack is not None and ack.route == route and ack.payload[:1] == b'\x01', ack)

    st = None
    for _ in range(20):
        f = port.ask(a.addr, SB.GETTING, 6, SB.ID_WIFI, timeout=0.5)
        if f and len(f.payload) == 47:
            st = f.payload
            if st[0] == 3:
                break
        time.sleep(0.3)
    if not check('relay state READY (Wi-Fi up, peer set)', st is not None and st[0] == 3, st and st[0]):
        return

    # ---- up: serial -> UDP
    rng = random.Random(5)
    sent = bytearray()
    frames = []
    for i in range(60):
        k = i % 4
        if k == 0:
            f = KF.kp1(0, SB.mode(SB.GETTING, 0, resp=True), 0x20 + (i % 8), bytes(rng.randrange(256) for _ in range(rng.randrange(0, 60))))
        elif k == 1:
            f = KF.kp2(bytes(rng.randrange(256) for _ in range(rng.choice([100, 600, 1400, 3000]))))
        elif k == 2:
            f = rng.choice([KF.kp1(3, SB.mode(SB.CONTENT, 1), 0x03, bytes(rng.randrange(256) for _ in range(255))),
                            KF.ubx(0x01, 0x07, bytes(rng.randrange(256) for _ in range(92))),
                            KF.mav1(0, bytes(9), seq=i & 0xFF),
                            KF.mav2(33, bytes(rng.randrange(256) for _ in range(28)), seq=i & 0xFF)])
        else:
            sent += b'$GPGGA,noise*00' + bytes([13, 10])
            continue
        frames.append(f)
        sent += f
    port.raw.clear()
    for i in range(0, len(sent), 200):
        port.p.write(sent[i:i + 200])
        time.sleep(0.003)
    got = []
    end = time.monotonic() + 3.0
    while time.monotonic() < end:
        try:
            d, src = udp.recvfrom(2048)
            got.append(d)
            module = src
        except socket.timeout:
            pass
    joined = b''.join(got)
    check('up: UDP packets concatenate to exactly what was sent (%d B in %d packets)' % (len(sent), len(got)), joined == sent,
          '%d vs %d bytes' % (len(joined), len(sent)))
    check('up: every packet <= 512 bytes', all(len(p) <= 512 for p in got), max(map(len, got)) if got else None)
    fr = KF.Framer(4096)
    fr.feed(sent)
    fr.flush(force=True)
    bounds, pos = set(), 0
    for p in got:
        pos += len(p)
        bounds.add(pos)
    pos, whole = 0, True
    for kind, d, _ in fr.out:
        if len(d) <= 512 and any(b in bounds for b in range(pos + 1, pos + len(d))):
            whole = False
        pos += len(d)
    check('up: no frame <= 512 B split across packets; longer ones cut at 512', whole)

    # ---- down: UDP -> serial
    if not got:
        return
    down = [KF.kp1(0, SB.mode(SB.CONTENT, 0), 0x02, struct.pack('<I', i)) for i in range(20)]
    down += [KF.mav2(24, bytes(30), seq=1), KF.ubx(0x01, 0x07, bytes(92)), KF.mav1(0, bytes(9), seq=2)]
    down.append(KF.kp2(bytes(range(256)) * 4))
    port.raw.clear()
    blob = b''.join(down)
    cut = len(blob) - 700
    udp.sendto(blob[:cut], module)       # the big frame arrives in two datagrams
    time.sleep(0.05)
    udp.sendto(blob[cut:], module)
    port.pump(1.5)
    fr = KF.Framer(4096)
    fr.feed(bytes(port.raw))
    fr.flush(force=True)
    relayed = [d for k, d, _ in fr.out if not (k == 'F' and d[0] == 0xBB and d[2] == a.addr)]
    check('down: serial output (without the module\'s own frames) == what the boat sent',
          b''.join(relayed) == blob, '%d vs %d bytes' % (len(b''.join(relayed)), len(blob)))
    check('down: the frame split across two datagrams arrives whole', any(k == 'F' and d == down[-1] for k, d, _ in fr.out))

    # ---- local frame is not relayed
    while True:
        try:
            udp.recvfrom(2048)
        except socket.timeout:
            break
    f = port.ask(a.addr, SB.GETTING, 0, SB.ID_WIFI, timeout=1.0)
    leaked = []
    end = time.monotonic() + 0.5
    while time.monotonic() < end:
        try:
            leaked.append(udp.recvfrom(2048)[0])
        except socket.timeout:
            pass
    check('local: ID_WIFI v0 to the module answered, nothing relayed', f is not None and not leaked, len(leaked))

    f = port.ask(a.addr, SB.GETTING, 6, SB.ID_WIFI, timeout=1.0)
    if f:
        v = struct.unpack('<10I', f.payload[7:47])
        print('stats: up frames %d bytes %d packets %d drops %d | down packets %d bytes %d frames %d drops %d | local %d' % v[:9])
        check('no drops on either side', v[3] == 0 and v[7] == 0, (v[3], v[7]))


if __name__ == '__main__':
    main()
