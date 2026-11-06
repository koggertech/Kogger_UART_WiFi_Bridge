#!/usr/bin/env python3
"""GODIO F8/F9 serial frames and their MAVLink translation: the Python mirror of firmware/main/f9.c and f9mav.c
(docs/GODIO_F9.md).

iter_frames() is the reference boundary rule (the decoder f9_decode.py the boat's captures were read with,
2026-10-07): at a sync byte the
shortest length 4..40 whose 8-bit sum is valid and that is followed by a sync or the end of the data. Stream is the
same rule fed in pieces, as f9_feed()/f9_end() run it.
"""
import math
import struct

import kframe as KF
import mavcrc_table

MIN_LEN, MAX_LEN = 4, 40
MIN_SATS = 4  # F9MAV_MIN_SATS: fewer satellites, no fix - no position (2026-10-07: 1 satellite, frozen position)
SYNC = (0xF8, 0xF9)


def cksum(b):
    return sum(b) & 0xFF


def frame_ok(d, i, length):
    if i + length > len(d):
        return False
    if cksum(d[i:i + length - 1]) != d[i + length - 1]:
        return False
    nxt = i + length
    return nxt == len(d) or d[nxt] in SYNC


def iter_frames(d):
    """The reference, as written in f9_decode.py: (offset, frame) of every frame in the whole data d."""
    i = 0
    while i < len(d):
        if d[i] in SYNC:
            found = None
            for length in range(MIN_LEN, MAX_LEN + 1):
                if frame_ok(d, i, length):
                    found = length
                    break
            if found:
                yield i, bytes(d[i:i + found])
                i += found
                continue
        i += 1


def frames_of(d):
    return [f for _, f in iter_frames(d)]


class Stream:
    """f9_feed()/f9_end(): frames decided as soon as the byte after them (or the end) is known."""

    def __init__(self):
        self.buf = bytearray()
        self.frames = []
        self.skipped = 0

    def _run(self, end):
        b = self.buf
        while b:
            if b[0] not in SYNC:
                del b[0]
                self.skipped += 1
                continue
            found = 0
            for l in range(MIN_LEN, MAX_LEN + 1):
                if l > len(b):
                    if not end:
                        return
                    break
                if cksum(b[:l - 1]) != b[l - 1]:
                    continue
                if l == len(b):
                    if not end:
                        return
                    found = l
                    break
                if b[l] in SYNC:
                    found = l
                    break
            if found:
                self.frames.append(bytes(b[:found]))
                del b[:found]
            else:
                del b[0]
                self.skipped += 1

    def feed(self, data):
        for x in data:
            self.buf.append(x)
            self._run(False)

    def end(self):
        self._run(True)


def pos(frame):
    """F9 C4 fields as float32 values (as the C reads them), or None."""
    if len(frame) != 19 or frame[0] != 0xF9 or frame[1] != 0xC4 or cksum(frame[:-1]) != frame[-1]:
        return None
    lon, lat = struct.unpack_from('<ff', frame, 2)
    dist, volt = struct.unpack_from('<HB', frame, 10)
    hdg, = struct.unpack_from('<f', frame, 13)
    return dict(lon=lon, lat=lat, dist=dist, volt_dv=volt, heading=hdg, sats=frame[17])


def round_i32(x):
    return int(math.floor(x + 0.5)) if x >= 0 else -int(math.floor(-x + 0.5))


def _f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def _frame(msgid, payload, seq):
    extra, base, _ = mavcrc_table.TABLE[msgid]
    assert len(payload) == base, (msgid, len(payload), base)
    return KF.mav2(msgid, payload, seq=seq & 0xFF, sysid=1, compid=1, extra=extra)


def position(p, now_us, seq):
    """(bytes, next seq) for one F9 C4, the order and fields of f9mav_position() (no ATTITUDE: that is C0's)."""
    lat, lon, h = p['lat'], p['lon'], p['heading']
    ok = (p['sats'] >= MIN_SATS and math.isfinite(lat) and math.isfinite(lon) and abs(lat) <= 90 and abs(lon) <= 180
          and lat != 0 and lon != 0)
    hok = math.isfinite(h)
    ilat = round_i32(lat * 1e7) if ok else 0
    ilon = round_i32(lon * 1e7) if ok else 0
    cdeg = 0xFFFF
    if hok:
        c = math.fmod(round_i32(h * 100.0), 36000)
        cdeg = int(c + 36000 if c < 0 else c)
    ms = (now_us // 1000) & 0xFFFFFFFF
    out = b''
    out += _frame(24, struct.pack('<QiiiHHHHBB', now_us, ilat, ilon, 0, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
                                  3 if ok else 1, p['sats']), seq)
    seq += 1
    if ok:
        out += _frame(33, struct.pack('<IiiiihhhH', ms, ilat, ilon, 0, 0, 0, 0, 0, cdeg), seq)
        seq += 1
    volts = [p['volt_dv'] * 100] + [0xFFFF] * 9
    out += _frame(147, struct.pack('<iih10HhBBBb', -1, -1, 0x7FFF, *volts, -1, 0, 0, 0, -1), seq)
    seq += 1
    return out, seq


def status_heading(frame):
    """Heading of a valid F9 C0 (float32, as the C reads it), or None."""
    if len(frame) != 19 or frame[0] != 0xF9 or frame[1] != 0xC0 or cksum(frame[:-1]) != frame[-1]:
        return None
    return struct.unpack_from('<f', frame, 13)[0]


def attitude(h, now_us, seq):
    """(bytes, next seq): f9mav_attitude() for one F9 C0; b'' when the heading is not finite."""
    if not math.isfinite(h):
        return b'', seq
    a = math.fmod(h, 360.0)
    if a > 180.0:
        a -= 360.0
    elif a <= -180.0:
        a += 360.0
    yaw = _f32(a * (math.pi / 180.0))
    if abs(yaw) < _f32(1e-6):
        yaw = _f32(1e-6)
    ms = (now_us // 1000) & 0xFFFFFFFF
    return _frame(30, struct.pack('<Iffffff', ms, 0, 0, yaw, 0, 0, 0), seq), seq + 1


def c0_front(frame, seq):
    """(bytes, next seq): f9mav_c0_front() - STATUSTEXT with bytes 2..11 and satellites of a valid F9 C0."""
    if status_heading(frame) is None:
        return b'', seq
    text = ('F9 C0 front ' + ' '.join('%02x' % b for b in frame[2:12]) + ' sats %d' % frame[17]).encode()
    return _frame(253, bytes([7]) + text.ljust(50, bytes(1)), seq), seq + 1


def heartbeat(seq):
    return _frame(0, struct.pack('<IBBBBB', 0, 11, 0, 0, 4, 3), seq), seq + 1


REAL_C4 = bytes.fromhex('F9C4330AF84293122A4200007C0000F0412618')  # boat, 2026-10-07
REAL_F8 = bytes.fromhex('F8C064641090')
REAL_C0 = bytes.fromhex('F9C0000100000000000000007C0000F041268D')  # boat, 2026-10-07: 12.4 V, heading 30.0, 38 sats
