#!/usr/bin/env python3
"""Ping through the bridge from the bench PC, no HeadUnit needed: the script plays the host side
(10.99.0.2), sends ICMP echo requests as FRAME_T_IP over the serial link and matches the replies.

  python tools/bench_ping.py --port COM3 10.99.0.1 10.0.0.1       # targets
  python tools/bench_ping.py --port COM3 --sweep 10.0.0.1-30       # find hosts behind the AP
  python tools/bench_ping.py --port COM3 -n 50 --size 1400 10.0.0.1

10.99.0.1 answers from the ESP's own lwIP (link + netif); anything else crosses NAPT into the Wi-Fi.
Exit code 0 if every listed target answered at least once.
"""
import argparse
import os
import struct
import sys
import time

import serial

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'host'))
import wbframe  # noqa: E402

HOST = '10.99.0.2'


def csum(data):
    if len(data) % 2:
        data += b'\x00'
    s = sum(struct.unpack('!%dH' % (len(data) // 2), data))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF


def ip2b(a):
    return bytes(int(x) for x in a.split('.'))


def echo_request(dst, ident, seq, size):
    payload = bytes((i * 7 + seq) & 0xFF for i in range(size))
    icmp = struct.pack('!BBHHH', 8, 0, 0, ident, seq) + payload
    icmp = icmp[:2] + struct.pack('!H', csum(icmp)) + icmp[4:]
    hdr = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(icmp), seq, 0, 64, 1, 0, ip2b(HOST), ip2b(dst))
    hdr = hdr[:10] + struct.pack('!H', csum(hdr)) + hdr[12:]
    return hdr + icmp, payload


def parse_reply(pkt):
    """Return (src, ident, seq, payload) for an ICMP echo reply, else None."""
    if len(pkt) < 28 or pkt[0] >> 4 != 4 or pkt[9] != 1:
        return None
    ihl = (pkt[0] & 15) * 4
    icmp = pkt[ihl:]
    if len(icmp) < 8 or icmp[0] != 0 or csum(icmp) != 0:
        return None
    ident, seq = struct.unpack('!HH', icmp[4:8])
    return '.'.join(str(b) for b in pkt[12:16]), ident, seq, icmp[8:]


class Link:
    def __init__(self, port, baud):
        self.p = serial.Serial()
        self.p.port, self.p.baudrate, self.p.timeout = port, baud, 0.002
        self.p.dtr = self.p.rts = True
        self.p.open()
        self.dec = wbframe.Decoder()
        self.ctl = []

    def ping(self, dst, ident, seq, size, timeout):
        pkt, payload = echo_request(dst, ident, seq, size)
        t0 = time.perf_counter()
        self.p.write(wbframe.encode(wbframe.T_IP, pkt))
        end = t0 + timeout
        while time.perf_counter() < end:
            for t, fr in self.dec.feed(self.p.read(self.p.in_waiting or 1)):
                if t == wbframe.T_CTL:
                    self.ctl.append(fr.decode('utf-8', 'replace'))
                    continue
                r = parse_reply(fr)
                if r and r[0] == dst and r[1] == ident and r[2] == seq:
                    return (time.perf_counter() - t0) * 1000, r[3] == payload
        return None, False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', required=True)
    ap.add_argument('--baud', type=int, default=921600)
    ap.add_argument('-n', type=int, default=5, help='pings per target')
    ap.add_argument('--size', type=int, default=56, help='ICMP payload bytes (max 1472)')
    ap.add_argument('--timeout', type=float, default=1.0)
    ap.add_argument('--sweep', help='a.b.c.X-Y: one ping to each address, list who answers')
    ap.add_argument('targets', nargs='*')
    a = ap.parse_args()
    link = Link(a.port, a.baud)
    ident = os.getpid() & 0xFFFF

    if a.sweep:
        base, rng = a.sweep.rsplit('.', 1)
        lo, hi = (int(x) for x in rng.split('-'))
        found = []
        for i in range(lo, hi + 1):
            rtt, _ = link.ping('%s.%d' % (base, i), ident, i, 16, 0.3)
            if rtt is not None:
                found.append('%s.%d (%.1f ms)' % (base, i, rtt))
        print('answered:', ', '.join(found) or 'nobody')

    all_ok = True
    for dst in a.targets:
        rtts, bad = [], 0
        for seq in range(1, a.n + 1):
            rtt, same = link.ping(dst, ident, seq, a.size, a.timeout)
            if rtt is not None:
                rtts.append(rtt)
                bad += 0 if same else 1
        loss = 100.0 * (a.n - len(rtts)) / a.n
        if rtts:
            print('%-12s %d/%d replies, loss %.0f%%, rtt min/avg/max %.1f/%.1f/%.1f ms, payload mismatches %d'
                  % (dst, len(rtts), a.n, loss, min(rtts), sum(rtts) / len(rtts), max(rtts), bad))
        else:
            print('%-12s no replies (%d sent)' % (dst, a.n))
            all_ok = False
    for l in link.ctl:
        print('ctl:', l)
    print('host decoder: ok=%d crc_err=%d discarded=%d' % (link.dec.frames_ok, link.dec.crc_errors, link.dec.discarded))
    sys.exit(0 if all_ok else 1)


if __name__ == '__main__':
    main()
