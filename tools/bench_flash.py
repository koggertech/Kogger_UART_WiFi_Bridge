#!/usr/bin/env python3
"""Bench flash + smoke test for a board WITHOUT auto-reset wiring (UART link variant).

  python tools/bench_flash.py --port COM3                 # wait for ROM bootloader, flash, wait for a reset, test
  python tools/bench_flash.py --port COM3 --no-flash      # only the smoke test (power-cycle the module when asked)

Flashing: the chip must be in the ROM bootloader: hold BOOT while applying power (or while pressing RESET on
boards that have one). Everything runs in
one stub session at 115200, because each esptool call without a reset leaves the stub at its own baud
rate and the next call cannot find it. After flashing the script keeps the port open at the link baud,
asks for a reset (power-cycle without BOOT), and checks what the firmware says. Exit code 0 = all checks PASS.
Needs esptool >= 5 and pyserial.
"""
import argparse
import os
import subprocess
import sys
import time

import serial

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'host'))
import wbframe  # noqa: E402

results = []


def check(name, cond, detail=''):
    results.append((name, bool(cond)))
    print('%s  %s%s' % ('PASS' if cond else 'FAIL', name, ('  -- ' + detail) if detail else ''), flush=True)
    return cond


def esptool(port, *args, timeout=600):
    cmd = [sys.executable, '-m', 'esptool', '--port', port, '--baud', '115200'] + list(args)
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=timeout)
    return r.returncode, r.stdout


def flash(port, build_dir, wait_s):
    with open(os.path.join(build_dir, 'flash_args')) as f:
        args = f.read().replace('--flash_mode', '--flash-mode').replace('--flash_freq', '--flash-freq') \
                       .replace('--flash_size', '--flash-size').split()
    print('waiting for the ROM bootloader on %s (hold BOOT, replug the module, release BOOT) ...' % port, flush=True)
    end = time.monotonic() + wait_s
    while True:
        rc, out = esptool(port, '--before', 'no-reset', '--after', 'no-reset-stub', 'chip-id', timeout=30)
        if rc == 0:
            print('\n'.join(l for l in out.splitlines() if l.startswith(('Chip type', 'MAC'))), flush=True)
            break
        if time.monotonic() > end:
            return check('bootloader answered', False, 'timeout %d s' % wait_s)
        time.sleep(2)
    for attempt in range(1, 4):
        rc, out = subprocess.run(
            [sys.executable, '-m', 'esptool', '--port', port, '--baud', '115200', '--before', 'no-reset-no-sync',
             '--after', 'no-reset-stub', 'write-flash'] + args,
            cwd=build_dir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=900).returncode, None
        if rc == 0:
            return check('flash written, hashes verified by esptool (attempt %d)' % attempt, True)
        print('write-flash attempt %d failed, retrying in the same stub session' % attempt, flush=True)
    return check('flash written', False)


class Link:
    def __init__(self, port, baud):
        self.p = serial.Serial()
        self.p.port, self.p.baudrate, self.p.timeout = port, baud, 0.05
        self.p.dtr = self.p.rts = True
        self.p.open()
        self.dec = wbframe.Decoder()
        self.lines = []
        self.raw = 0

    def pump(self):
        data = self.p.read(4096)
        self.raw += len(data)
        for t, payload in self.dec.feed(data):
            if t == wbframe.T_CTL:
                line = payload.decode('utf-8', 'replace')
                self.lines.append(line)

    def wait_line(self, pred, timeout):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            for i, l in enumerate(self.lines):
                if pred(l):
                    del self.lines[i]
                    return l
            self.pump()
        return None

    def cmd(self, tag, text, timeout=5.0):
        """Send '<tag> <text>', return (final line, intermediate lines)."""
        self.p.write(wbframe.encode(wbframe.T_CTL, ('%s %s' % (tag, text)).encode()))
        inter = []
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            l = self.wait_line(lambda x: x.startswith(tag + ' '), max(0.05, end - time.monotonic()))
            if l is None:
                break
            rest = l[len(tag) + 1:]
            if rest.startswith(('OK', 'ERR')):
                return rest, inter
            inter.append(rest)
        return None, inter


def fields(rest):
    return wbframe.parse_fields(rest.split()[1:])


def smoke(port, baud, reset_wait):
    link = Link(port, baud)
    print('port %s open at %d. Reset the module now: power-cycle it WITHOUT BOOT (or press RESET if the board has one) ...' % (port, baud), flush=True)
    hello = link.wait_line(lambda l: l.startswith('* HELLO'), reset_wait)
    if not check('* HELLO after reset', hello is not None, hello or 'raw bytes seen: %d' % link.raw):
        return
    st = link.wait_line(lambda l: l.startswith('* STATE'), 5)
    check('* STATE after HELLO', st is not None, st or '')

    r, _ = link.cmd('1', 'VER')
    check('VER -> OK proto=1', r is not None and r.startswith('OK') and fields(r).get('proto') == '1', r or 'no reply')
    r, _ = link.cmd('2', 'STATUS')
    check('STATUS -> OK state=...', r is not None and r.startswith('OK') and 'state' in fields(r), r or 'no reply')
    r, nets = link.cmd('3', 'SCAN', timeout=15)
    ok = r is not None and r.startswith('OK') and fields(r).get('count') == str(len(nets))
    check('SCAN -> %d NET lines + OK count' % len(nets), ok, r or 'no reply')
    for n in nets[:15]:
        f = fields(n)
        try:
            ssid = wbframe.pct_decode(f.get('ssid', '')).decode('utf-8', 'replace')
        except ValueError:
            ssid = '?'
        print('      %-32s rssi=%-4s ch=%-3s auth=%s' % (ssid, f.get('rssi'), f.get('ch'), f.get('auth')))
    r, known = link.cmd('4', 'LIST')
    check('LIST -> OK count', r is not None and r.startswith('OK') and fields(r).get('count') == str(len(known)),
          r or 'no reply')
    r, _ = link.cmd('5', 'FOO')
    check('unknown command -> ERR UNKNOWN_CMD', r == 'ERR UNKNOWN_CMD', r or 'no reply')

    # Robustness: junk, bad CRC, short IP frame, bad control line; the module must stay up (no new HELLO).
    link.p.write(b'garbage without delimiters' + b'\xc0\x02' + b'1 VER' + b'\x00\x00\xc0'
                 + wbframe.encode(wbframe.T_IP, b'\x45' + bytes(10)))
    link.p.write(wbframe.encode(wbframe.T_CTL, b'onlyone'))
    bad = link.wait_line(lambda l: l.startswith('? ERR'), 3)
    check('unparsable line -> ? ERR BAD_LINE', bad is not None and 'BAD_LINE' in bad, bad or 'no reply')
    r, _ = link.cmd('6', 'STATS')
    f = fields(r) if r else {}
    check('STATS after junk: still up, crc/disc counted', r is not None and int(f.get('link_crc', 0)) >= 1
          and int(f.get('ip_bad', 0)) >= 1, r or 'no reply')
    again = link.wait_line(lambda l: l.startswith('* HELLO'), 1)
    check('no reset during the test', again is None, again or '')
    print('decoder: ok=%d crc_err=%d discarded=%d' % (link.dec.frames_ok, link.dec.crc_errors, link.dec.discarded))
    link.p.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', required=True)
    ap.add_argument('--baud', type=int, default=921600, help='link baud of the firmware')
    ap.add_argument('--build-dir', default=os.path.join(ROOT, 'firmware', 'build-uart'))
    ap.add_argument('--no-flash', action='store_true')
    ap.add_argument('--wait', type=int, default=900, help='seconds to wait for the bootloader and for the reset')
    a = ap.parse_args()
    if not a.no_flash and not flash(a.port, a.build_dir, a.wait):
        sys.exit(1)
    smoke(a.port, a.baud, a.wait)
    failed = [n for n, ok in results if not ok]
    print('\n%d checks, %d failed' % (len(results), len(failed)))
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
