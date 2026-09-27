#!/usr/bin/env python3
"""Control client for the ESP32-C3 Wi-Fi bridge.

  espwifi_ctl.py status                 # via the daemon on 127.0.0.1:5550 (HeadUnit)
  espwifi_ctl.py --serial COM7 scan     # directly over the serial port (bench, needs pyserial)

Commands: ver, status, scan, connect SSID [PASS] [--no-save], disconnect, auto, list,
forget SSID, stats, link (daemon only), reboot, watch, raw "<CMD> k=v ...".
Exit code 0 on OK, 1 on ERR/timeout. See docs/PROTOCOL.md.
"""
import argparse
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wbframe  # noqa: E402


class TcpChan:
    def __init__(self, addr):
        host, port = addr.rsplit(':', 1)
        self.s = socket.create_connection((host, int(port)), timeout=3)
        self.buf = b''

    def send(self, line):
        self.s.sendall(line.encode('utf-8') + b'\n')

    def recv(self, timeout):
        self.s.settimeout(timeout)
        while b'\n' not in self.buf:
            try:
                d = self.s.recv(4096)
            except socket.timeout:
                return None
            if not d:
                raise SystemExit('daemon closed the connection')
            self.buf += d
        line, self.buf = self.buf.split(b'\n', 1)
        return line.decode('utf-8', 'replace')


class SerialChan:
    def __init__(self, port, baud):
        import serial  # pyserial, bench use only
        self.p = serial.Serial()
        self.p.port, self.p.baudrate, self.p.timeout = port, baud, 0.05
        self.p.dtr = self.p.rts = True  # both asserted: no reset
        self.p.open()
        self.dec = wbframe.Decoder()
        self.lines = []

    def send(self, line):
        self.p.write(wbframe.encode(wbframe.T_CTL, line.encode('utf-8')))

    def recv(self, timeout):
        end = time.monotonic() + timeout
        while not self.lines:
            if time.monotonic() > end:
                return None
            for t, payload in self.dec.feed(self.p.read(4096)):
                if t == wbframe.T_CTL:
                    self.lines.append(payload.decode('utf-8', 'replace'))
        return self.lines.pop(0)


def pretty(line):
    """Decode percent-encoded values of known text fields for display."""
    out = []
    for tok in line.split(' '):
        k, sep, v = tok.partition('=')
        if sep and k in ('ssid', 'port'):
            try:
                v = wbframe.pct_decode(v).decode('utf-8', 'replace')
            except ValueError:
                pass
            tok = '%s="%s"' % (k, v)
        out.append(tok)
    return ' '.join(out)


def transact(ch, cmdline, timeout, verbose_events):
    tag = str(int(time.time() * 1000) % 100000)
    ch.send('%s %s' % (tag, cmdline))
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        line = ch.recv(max(0.05, end - time.monotonic()))
        if line is None:
            break
        t, _, rest = line.partition(' ')
        if t == tag:
            print(pretty(rest))
            if rest.startswith('OK'):
                return 0
            if rest.startswith('ERR'):
                return 1
        elif verbose_events:
            print(pretty(line))
    print('timeout', file=sys.stderr)
    return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--tcp', default='127.0.0.1:5550')
    ap.add_argument('--serial', help='talk to the ESP directly (bench), e.g. COM7 or /dev/ttyACM0')
    ap.add_argument('--baud', type=int, default=921600)
    ap.add_argument('-e', '--events', action='store_true', help='also print * events while waiting')
    ap.add_argument('cmd')
    ap.add_argument('args', nargs='*')
    ap.add_argument('--no-save', action='store_true')
    a = ap.parse_args()

    ch = SerialChan(a.serial, a.baud) if a.serial else TcpChan(a.tcp)
    c = a.cmd.lower()
    if c == 'watch':
        while True:
            line = ch.recv(3600)
            if line is not None:
                print(time.strftime('%H:%M:%S'), pretty(line), flush=True)
    simple = {'ver': 'VER', 'status': 'STATUS', 'disconnect': 'DISCONNECT', 'auto': 'AUTO',
              'list': 'LIST', 'stats': 'STATS', 'link': 'LINK', 'reboot': 'REBOOT'}
    if c in simple:
        line, timeout = simple[c], 5
    elif c == 'scan':
        line, timeout = 'SCAN', 15
    elif c == 'connect' and 1 <= len(a.args) <= 2:
        line = 'CONNECT ssid=%s' % wbframe.pct_encode(a.args[0])
        if len(a.args) == 2:
            line += ' pass=%s' % wbframe.pct_encode(a.args[1])
        if a.no_save:
            line += ' save=0'
        timeout = 5
    elif c == 'forget' and len(a.args) == 1:
        line, timeout = 'FORGET ssid=%s' % wbframe.pct_encode(a.args[0]), 5
    elif c == 'raw' and a.args:
        line, timeout = ' '.join(a.args), 15
    else:
        ap.error('unknown command or wrong arguments')
    sys.exit(transact(ch, line, timeout, a.events))


if __name__ == '__main__':
    main()
