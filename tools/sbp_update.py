#!/usr/bin/env python3
"""Update the Wi-Fi module over Kogger SBP exactly the way KoggerApp does it, and check the result.

  python tools/sbp_update.py --port COM3 dist/test/KoggerWiFi_0.99.0.ufww  # the last check reboots: proves it confirmed
  python tools/sbp_update.py --port COM3 FILE --drop 7          # chunk 7 lost once: must recover
  python tools/sbp_update.py --port COM3 FILE --corrupt 300     # a byte flipped in chunk 300: must be refused
  python tools/sbp_update.py --port COM3 FILE --stop 50         # transfer abandoned: old image keeps running
  python tools/sbp_update.py --port COM3 FILE --expect-rollback # image that never confirms: must roll back
  python tools/sbp_update.py --port /dev/ttyUSB0 --route 87 --sbp-only FILE   # a line bridges: SBP-only port
  python tools/sbp_update.py --port /dev/ttyUSB0 --route 88 --sbp-only --back-wait 90 FILE  # over the air (0.17+)

--sbp-only is for a module whose port carries only SBP (a UART line bridges to the network, 0.7+): the
SLIP questions of fwinfo.py cannot reach it there. Versions are then read with ID_VERSION (major.minor only,
so the file must differ in the second number) and confirmation is proven by a reboot through ID_BOOT v0:
an image that has not confirmed itself is rolled back by the bootloader at that reboot, and a reset uptime
shows the reboot really happened. Never switch the bridge off to make SLIP answer: on 0.7/0.8 a module
with the bridge off and Wi-Fi connected keeps rebooting (docs/RELAY.md).

A new image confirms itself after 60 s of work with the host talking (0.11; 5 s before), so the script
keeps asking for that long before its proving reboot. The port rate must be the saved one (ID_FLASH):
the reboots come back at the saved rate.

Host side of the procedure (KoggerApp src/device/dev_driver.cpp): ID_BOOT v0 + key ->
poll ID_MARK + ID_VERSION until the mark bit is back and bootMode = 1 -> ID_UPDATE v0 chunks
(U2 number from 1, 96 bytes), one answer per chunk, a second chunk in flight from chunk 3 on,
reposition on type 2, abort on type > 2, resend the same chunk after 2 s (5 times) -> ID_BOOT v1 +
key as soon as the image is exhausted. Exit code 0 = all checks PASS.
"""
import argparse
import os
import struct
import sys
import time

import serial

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'host'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import sbpframe as SB  # noqa: E402
import wbframe  # noqa: E402
from make_update import inspect  # noqa: E402
import fwinfo  # noqa: E402

ID_UPDATE = 0x25
KEY = SB.KEY_CONFIRM.to_bytes(4, 'little')
CHUNK = 96
CONFIRM_WAIT_S = 65      # the image confirms after 60 s with the host talking (0.11)
ROLLBACK_WAIT_S = 240    # an image that never confirms reboots after CONFIG_WB_OTA_CONFIRM_S (180 s)
results = []


def check(name, cond, detail=''):
    results.append((name, bool(cond)))
    print('%s  %s%s' % ('PASS' if cond else 'FAIL', name, ('  -- ' + str(detail)) if detail else ''), flush=True)
    return cond


class Dev:
    def __init__(self, port, baud, route):
        self.p = serial.Serial()
        self.p.port, self.p.baudrate, self.p.timeout = port, baud, 0.002
        self.p.dtr = self.p.rts = True
        self.p.open()
        self.route = route
        self.dec = SB.Decoder()
        self.rx = []

    def pump(self, sec=0.0):
        end = time.monotonic() + sec
        while True:
            self.rx += self.dec.feed(self.p.read(self.p.in_waiting or 1))
            if time.monotonic() >= end:
                return

    def send(self, ftype, ver, id_, payload=b''):
        """Returns the frame's checksum, which the acknowledgement echoes."""
        f = SB.encode(self.route, SB.mode(ftype, ver, resp=ftype != SB.CONTENT), id_, payload)
        self.p.write(f)
        return f[-2], f[-1]

    def acked(self, id_, ver, ck, timeout):
        """Code of the acknowledgement of exactly this request (version and checksum echo), or None."""
        a = self.wait(lambda f: f.id == id_ and f.ver == ver and f.type == SB.CONTENT and f.resp and
                      len(f.payload) >= 3 and (f.payload[1], f.payload[2]) == ck, timeout)
        return a.payload[0] if a else None

    def take(self, pred):
        for i, f in enumerate(self.rx):
            if pred(f):
                return self.rx.pop(i)
        return None

    def wait(self, pred, timeout):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            f = self.take(pred)
            if f:
                return f
            self.pump(0.01)
        return None

    def version(self, timeout=1.0):
        """(bootMode, 'major.minor', mark) from ID_VERSION v2, or None. Only the module's answer counts
        (board 87): a device behind a bridging module may answer a request to 0 too. Since 0.12 the module
        answers ID_VERSION to 0 from its own address (discovery), so the route of the answer becomes the
        route of every later request: an update from 0.11 at address 0 to 0.12 at 87 follows the module."""
        self.rx.clear()
        self.send(SB.GETTING, 2, SB.ID_VERSION)
        f = self.wait(lambda f: f.id == SB.ID_VERSION and f.ver == 2 and f.type == SB.CONTENT and not f.resp and
                      len(f.payload) == 9 and f.payload[2] == SB.BOARD_WIFI, timeout)
        if not f:
            return None
        if f.route != self.route:
            print('module answers from address %d' % f.route)
            self.route = f.route
        return f.payload[0], '%d.%d' % (f.payload[8], f.payload[7]), f.mark


def uptime(d, timeout=1.5):
    """Seconds since boot from the ID_WIFI v1 link report (fresh: older reports are dropped first)."""
    d.rx = [f for f in d.rx if not (f.id == SB.ID_WIFI and f.ver == 1)]
    d.send(SB.GETTING, 1, SB.ID_WIFI)
    f = d.wait(lambda f: f.id == SB.ID_WIFI and f.ver == 1 and f.type == SB.CONTENT and not f.resp, timeout)
    return SB.parse_link(f.payload)['uptime'] if f else None


def reboot_report(d, since, what):
    """How the module came back after a reboot request: version, uptime, the reason of its last reset and how long
    after the request it booted. An image that dies early comes back on the old one with PANIC or a watchdog as the
    reason; a bootloader that did not take the new image comes back with SW, booted right after the request."""
    d.rx = [f for f in d.rx if not (f.id == SB.ID_WIFI and f.ver == 1)]
    d.send(SB.GETTING, 1, SB.ID_WIFI)
    f = d.wait(lambda f: f.id == SB.ID_WIFI and f.ver == 1 and f.type == SB.CONTENT and not f.resp, 2.0)
    v = d.version(1.0)
    if not f:
        print('      after %s: no link report (version %s)' % (what, v))
        return None
    link = SB.parse_link(f.payload)
    booted = time.monotonic() - since - link['uptime']
    print('      after %s: version %s, uptime %d s, last reset %s, booted %.0f s after the request' % (
          what, v and v[1], link['uptime'], link.get('reset'), booted))
    return link


def wait_confirmed(d):
    """Keep talking to the new image (the host evidence) until it has run long enough to confirm."""
    print('keeping the new image busy until it has run %d s (it confirms itself after 60 s) ...' % CONFIRM_WAIT_S)
    end = time.monotonic() + CONFIRM_WAIT_S + 40
    up = None
    while time.monotonic() < end:
        up = uptime(d)
        if up is not None and up >= CONFIRM_WAIT_S:
            return up
        time.sleep(1.0)
    return up


def ver_mm(v):
    parts = v.split('.')
    return '%d.%d' % (int(parts[0]), int(parts[1]))


def poll_version(d, want, timeout):
    end = time.monotonic() + timeout
    last = None
    while time.monotonic() < end:
        v = d.version(0.5)
        if v:
            last = v
            if want is None or v[1] == want:
                return v
        time.sleep(0.2)
    return last


def upgrade(d, img, a):
    """Returns (transfer_ok, ack code of ID_BOOT v1 or None, seconds)."""
    d.rx.clear()
    d.send(SB.SETTING, 0, SB.ID_BOOT, KEY)
    end = time.monotonic() + 8.0
    ready = False
    while time.monotonic() < end and not ready:
        d.send(SB.SETTING, 0, SB.ID_MARK, KEY)
        d.send(SB.GETTING, 2, SB.ID_VERSION)
        f = d.wait(lambda f: f.id == SB.ID_VERSION and f.ver == 2 and f.type == SB.CONTENT and not f.resp, 0.2)
        ready = f is not None and f.mark and len(f.payload) == 9 and f.payload[0] == 1
    if not check('update window: mark bit back and bootMode = 1', ready):
        return False, None, 0
    time.sleep(0.1)                                   # KoggerApp: putUpdate() 100 ms after in_update

    num, off = 1, 0                                   # next chunk to send
    dropped, t0 = set(), time.monotonic()
    last_sent = None                                  # (num, off) of the newest chunk put on the wire
    resends, repositions = 0, 0

    def put():
        """KoggerApp putUpdate(): send chunk (num, off), advance; False when the image is exhausted."""
        nonlocal num, off, last_sent
        n = min(CHUNK, len(img) - off)
        if n <= 0:
            return False
        data = bytearray(img[off:off + n])
        if a.corrupt == num:
            data[n // 2] ^= 0x01
        if a.drop == num and num not in dropped:
            dropped.add(num)                          # lost on the wire, the host believes it was sent
        else:
            d.send(SB.SETTING, 0, ID_UPDATE, struct.pack('<H', num) + bytes(data))
        last_sent = (num, off)
        num += 1
        off += n
        return True

    def process():
        """KoggerApp fwUpgradeProcess(): one chunk per answer, a second one when chunk 3 becomes next."""
        more = put()
        if more and num == 3:
            more = put()
        return more

    process()
    last_answer = time.monotonic()
    while True:
        if a.stop and num > a.stop:
            print('stopping the transfer after chunk %d (test)' % a.stop)
            return False, None, time.monotonic() - t0
        f = d.take(lambda f: f.id == ID_UPDATE and f.type == SB.CONTENT and not f.resp)
        if f is None:
            d.pump(0.005)
            if time.monotonic() - last_answer > 2.0:  # packetAnswerTimeoutMsec: resend the same point
                resends += 1
                if resends > 5:
                    check('transfer', False, 'no answer after 5 resends')
                    return False, None, time.monotonic() - t0
                num, off = last_sent
                process()
                last_answer = time.monotonic()
            continue
        last_num, last_off, typ, rcv = struct.unpack('<HIBB', f.payload[:8])
        last_answer = time.monotonic()
        resends = 0
        if typ > 2:
            check('transfer', False, 'device fatal type %d at chunk %d (%d bytes stored)' % (typ, rcv, last_off))
            return False, typ, time.monotonic() - t0
        if typ == 2:
            repositions += 1
            num, off = last_num + 1, last_off
        if not process():
            # image exhausted: runFW at once, as KoggerApp does. The acknowledgement of ID_BOOT v0 from the
            # start is still queued: only an answer to this very frame counts (it did not up to 0.10).
            d.rx = [f for f in d.rx if f.id != SB.ID_BOOT]
            ck = d.send(SB.SETTING, 1, SB.ID_BOOT, KEY)
            code = d.acked(SB.ID_BOOT, 1, ck, 3.0)
            dt = time.monotonic() - t0
            print('transfer done: %d bytes in %.1f s (%.1f KB/s), repositions %d, ID_BOOT v1 -> %s' % (
                len(img), dt, len(img) / dt / 1024, repositions, SB.RESP.get(code, code)))
            return True, code, dt


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', required=True)
    ap.add_argument('--baud', type=int, default=921600)
    ap.add_argument('--route', type=int, default=0,
                    help='address to ask first (default 0: 0.12 answers it from its own address; a 0.11 module '
                         'whose line bridges needs its bridging address, e.g. 87)')
    ap.add_argument('--drop', type=int, default=0, help='lose chunk N once')
    ap.add_argument('--corrupt', type=int, default=0, help='flip one byte in chunk N (image must be refused)')
    ap.add_argument('--stop', type=int, default=0, help='abandon the transfer after chunk N')
    ap.add_argument('--expect-rollback', action='store_true', help='image never confirms: expect the old version back')
    ap.add_argument('--back-wait', type=float, default=20.0,
                    help='seconds to wait for the module after each of its reboots; a module behind a Wi-Fi link needs '
                         'more (its station must notice the access point restart and join again): use 90')
    ap.add_argument('--sbp-only', action='store_true',
                    help='the port carries only SBP (a line bridges): no SLIP questions, confirmation proven by a reboot')
    ap.add_argument('file')
    a = ap.parse_args()

    img = open(a.file, 'rb').read()
    try:
        new_ver, _ = inspect(img)
    except ValueError as e:
        sys.exit('refused before sending: %s' % e)
    new_mm = ver_mm(new_ver)

    # Full version/slot/state over SLIP (ID_VERSION only carries major.minor). May reboot the module.
    if a.sbp_only:
        before, old_full = None, None
    else:
        before, _ = fwinfo.info(a.port, a.baud, a.route)
        if not check('full version before: %s' % (before and '%s %s %s' % (before.get('fw'), before.get('part'), before.get('ota'))),
                     before is not None and before.get('ota') != 'pending'):
            sys.exit(1)
        old_full = before['fw']

    d = Dev(a.port, a.baud, a.route)
    if not a.sbp_only:  # with a bridging line this SLIP frame would be relayed to the peer as raw bytes
        d.p.write(wbframe.encode(wbframe.T_CTL, b'0 PROTO link=sbp'))   # no-op unless locked to SLIP
    d.pump(0.3)
    v = poll_version(d, None, 3.0)
    if not check('module answers ID_VERSION', v is not None):
        sys.exit(1)
    old_mm = v[1]
    if old_full is None:
        old_full = old_mm
    print('module runs %s, file is %s (%d bytes)' % (old_full, new_ver, len(img)))
    if not a.stop and not a.corrupt and new_mm == old_mm:
        if a.sbp_only:
            sys.exit('same major.minor as the running image (%s): over SBP alone a failed update would look '
                     'like a successful one. Build the file with a higher second number.' % old_mm)
        print('NOTE: same major.minor as the running image: KoggerApp cannot tell them apart; '
              'the full check below still can')

    ok, code, _ = upgrade(d, img, a)
    expect = old_full
    if a.stop:
        time.sleep(0.5)
        v = poll_version(d, None, 3.0)
        check('abandoned transfer: module still answers', v is not None, v)
        print('waiting 32 s for the device-side transfer timeout ...')
        time.sleep(32)
        v = poll_version(d, None, 3.0)
        check('after the timeout: bootMode 0, still %s' % old_mm, v is not None and v[0] == 0 and v[1] == old_mm, v)
    elif a.corrupt:
        check('corrupted image refused at ID_BOOT v1 (ERR_RUNTIME)', code == 8, code)
        time.sleep(1.0)
        v = poll_version(d, None, 3.0)
        check('module keeps running %s, bootMode 0' % old_mm, v is not None and v[1] == old_mm and v[0] == 0, v)
    else:
        check('image accepted (ID_BOOT v1 -> OK)', ok and code == 1, code)
        t_boot = time.monotonic()
        v = poll_version(d, new_mm, a.back_wait)
        reboot_report(d, t_boot, 'ID_BOOT v1')
        check('module comes back reporting %s' % new_mm, v is not None and v[1] == new_mm, v)
        if a.expect_rollback:
            print('image must not confirm; waiting for the rollback (up to %d s) ...' % ROLLBACK_WAIT_S)
            v = poll_version(d, old_mm, ROLLBACK_WAIT_S)
            check('rolled back: reports %s again' % old_mm, v is not None and v[1] == old_mm, v)
        else:
            up = wait_confirmed(d)                     # our frames are the host evidence
            check('new image ran %s s with the host talking' % up, up is not None and up >= CONFIRM_WAIT_S, up)
            expect = new_ver
    if a.sbp_only:
        # ID_BOOT v0 opens the update window; nothing comes, so the module reboots after 5 s. An image that
        # has not confirmed itself is rolled back by the bootloader at this reboot.
        want = new_mm if expect == new_ver else old_mm
        d.rx.clear()
        ck = d.send(SB.SETTING, 0, SB.ID_BOOT, KEY)
        code = d.acked(SB.ID_BOOT, 0, ck, 2.0)
        check('proving reboot requested (ID_BOOT v0 -> OK)', code == 1, code)
        t0 = time.monotonic()
        time.sleep(7.0)
        v = poll_version(d, want, a.back_wait)
        reboot_report(d, t0, 'the proving reboot')
        up = uptime(d)
        rebooted = up is not None and up <= time.monotonic() - t0 + 2
        check('after the reboot (uptime %s s) the module reports %s: %s' % (
              up, want, 'confirmed' if want == new_mm else 'unchanged'),
              v is not None and v[1] == want and v[0] == 0 and rebooted, v)
        d.p.close()
        failed = [n for n, ok_ in results if not ok_]
        print('\n%d checks, %d failed' % (len(results), len(failed)))
        sys.exit(1 if failed else 0)
    d.p.close()
    # The final question reboots the module: an unconfirmed image would be rolled back right here.
    after, rebooted = fwinfo.info(a.port, a.baud, a.route)
    got = after and '%s %s %s' % (after.get('fw'), after.get('part'), after.get('ota'))
    ok_after = after is not None and after.get('fw') == expect and after.get('ota') in ('valid', 'undefined')
    if expect == new_ver:
        ok_after = ok_after and after.get('part') != before.get('part')
    else:
        ok_after = ok_after and after.get('part') == before.get('part')
    check('full version after%s: expect %s, got %s' % (' a reboot' if rebooted else '', expect, got), ok_after)
    d.p.close()
    failed = [n for n, ok_ in results if not ok_]
    print('\n%d checks, %d failed' % (len(results), len(failed)))
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
