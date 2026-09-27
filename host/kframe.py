"""Python mirror of firmware/main/kframe.c and kpack.c (relay framing), for tests/test_all.py.

Units: ('F', bytes, proto) whole frame with a correct checksum, proto 1 KP1, 2 KP2, 3 UBX, 4 MAVLink 1,
5 MAVLink 2; ('R', bytes, 0) raw bytes. Frame builders for tests: kp1, kp2, ubx, mav1, mav2.
"""
from mavcrc_table import TABLE as MAV

RAW_RUN = 512
BUDGET_RATIO, BUDGET_SAVE = 8, 32  # rescan budget, as KF_BUDGET_RATIO / KF_BUDGET_SAVE
P_KP1, P_KP2, P_UBX, P_MAV1, P_MAV2 = 1, 2, 3, 4, 5


def fletcher(data):
    c1 = c2 = 0
    for b in data:
        c1 = (c1 + b) & 0xFF
        c2 = (c2 + c1) & 0xFF
    return c1, c2


def x25(data, crc=0xFFFF):
    for b in data:
        t = (b ^ crc) & 0xFF
        t = (t ^ (t << 4)) & 0xFF
        crc = ((crc >> 8) ^ (t << 8) ^ (t << 3) ^ (t >> 4)) & 0xFFFF
    return crc


def kp1(route, mode, id_, payload):
    body = bytes([route, mode, id_, len(payload)]) + bytes(payload)
    return b'\xbb\x55' + body + bytes(fletcher(body))


def kp2(body):
    """KP2 frame: CC 55 | U2 total length | body | Fletcher over [2, total-2)."""
    total = 4 + len(body) + 2
    head = bytes([0xCC, 0x55, total & 0xFF, total >> 8])
    return head + bytes(body) + bytes(fletcher(head[2:] + bytes(body)))


def ubx(cls, id_, payload):
    body = bytes([cls, id_, len(payload) & 0xFF, len(payload) >> 8]) + bytes(payload)
    return b'\xb5\x62' + body + bytes(fletcher(body))


def mav1(msgid, payload, seq=0, sysid=1, compid=1, extra=None):
    e = MAV[msgid][0] if extra is None else extra
    body = bytes([len(payload), seq, sysid, compid, msgid]) + bytes(payload)
    crc = x25(bytes([e]), x25(body))
    return b'\xfe' + body + bytes([crc & 0xFF, crc >> 8])


def mav2(msgid, payload, seq=0, sysid=1, compid=1, signed=False, extra=None):
    e = MAV[msgid][0] if extra is None else extra
    body = bytes([len(payload), 1 if signed else 0, 0, seq, sysid, compid,
                  msgid & 0xFF, (msgid >> 8) & 0xFF, msgid >> 16]) + bytes(payload)
    crc = x25(bytes([e]), x25(body))
    return b'\xfd' + body + bytes([crc & 0xFF, crc >> 8]) + (bytes(range(13)) if signed else b'')


class Framer:
    """Byte-for-byte the state machine of kf_feed()/kf_flush()."""

    def __init__(self, cap):
        self.cap = cap
        self.buf = bytearray()
        self.need = 0
        self.proto = 0
        self.extra = 0
        self.pb = bytearray()
        self.raw = bytearray()
        self.out = []
        self.budget = BUDGET_SAVE * cap
        self.no_rescan = 0

    def _raw_emit(self):
        if self.raw:
            self.out.append(('R', bytes(self.raw), 0))
            self.raw = bytearray()

    def _raw_add(self, b):
        self.raw.append(b)
        if len(self.raw) == RAW_RUN:
            self._raw_emit()

    def _reject(self):
        n = len(self.buf) - 1
        if n > self.budget:  # rescanning keeps failing: the candidate goes out raw, not rescanned
            for b in self.buf:
                self._raw_add(b)
            self.no_rescan += 1
            self.buf = bytearray()
            self.need = 0
            self.proto = 0
            return
        self.budget -= n
        self._raw_add(self.buf[0])
        self.pb[0:0] = self.buf[1:]
        self.buf = bytearray()
        self.need = 0
        self.proto = 0

    def _header(self):
        h, n = self.buf, len(self.buf)
        if self.proto == P_KP1:
            if n >= 6:
                self.need = h[5] + 8
            return True
        if self.proto == P_KP2:
            if n < 4:
                return True
            self.need = h[2] | (h[3] << 8)
            return 7 <= self.need <= self.cap
        if self.proto == P_UBX:
            if n < 6:
                return True
            self.need = (h[4] | (h[5] << 8)) + 8
            return self.need <= self.cap
        if self.proto == P_MAV1:
            if n < 6:
                return True
            e = MAV.get(h[5])
            if not e or h[1] != e[1]:
                return False
            self.extra = e[0]
            self.need = h[1] + 8
            return True
        if self.proto == P_MAV2:
            if n < 10:
                return True
            e = MAV.get(h[7] | (h[8] << 8) | (h[9] << 16))
            if (h[2] & ~1) or not e or h[1] == 0 or h[1] > e[2]:
                return False
            self.extra = e[0]
            self.need = h[1] + 12 + (13 if h[2] & 1 else 0)
            return True
        return False

    def _frame_ok(self):
        f = self.buf
        if self.proto in (P_KP1, P_KP2, P_UBX):
            c1, c2 = fletcher(f[2:self.need - 2])
            return c1 == f[self.need - 2] and c2 == f[self.need - 1]
        at = self.need - 2 if self.proto == P_MAV1 else f[1] + 10
        crc = x25(bytes([self.extra]), x25(f[1:at]))
        return f[at] == (crc & 0xFF) and f[at + 1] == (crc >> 8)

    def _step(self, b):
        if not self.buf:
            if b in (0xBB, 0xCC, 0xB5):
                self.proto = 0
            elif b == 0xFE:
                self.proto = P_MAV1
            elif b == 0xFD:
                self.proto = P_MAV2
            else:
                self._raw_add(b)
                return
            self.buf.append(b)
            return
        if len(self.buf) == 1 and self.proto == 0:
            s = self.buf[0]
            p = P_KP1 if (s == 0xBB and b == 0x55) else P_KP2 if (s == 0xCC and b == 0x55) else \
                P_UBX if (s == 0xB5 and b == 0x62) else 0
            if p:
                self.proto = p
                self.buf.append(b)
                return
            self.buf = bytearray()
            self._raw_add(s)
            self._step(b)
            return
        self.buf.append(b)
        if self.need == 0 and not self._header():
            self._reject()
            return
        if self.need and len(self.buf) == self.need:
            if self._frame_ok():
                self._raw_emit()
                self.out.append(('F', bytes(self.buf), self.proto))
                self.buf = bytearray()
                self.need = 0
                self.proto = 0
            else:
                self._reject()

    def feed(self, data):
        data = bytearray(data)
        i = 0
        while True:
            if self.pb:
                b = self.pb.pop(0)
            elif i < len(data):
                b = data[i]
                i += 1
                if self.budget < BUDGET_SAVE * self.cap:
                    self.budget += BUDGET_RATIO
            else:
                break
            self._step(b)

    def flush(self, force=False):
        while force and self.buf:  # give up the overdue candidate byte by byte, rescanning the rest
            self._reject()
            self.feed(b'')
        self._raw_emit()


class Packer:
    """Mirror of kpack.c; packets collected in .packets."""

    def __init__(self, maxlen=512):
        self.max = maxlen
        self.buf = bytearray()
        self.packets = []

    def flush(self):
        if self.buf:
            self.packets.append(bytes(self.buf))
            self.buf = bytearray()

    def unit(self, d):
        if len(d) <= self.max - len(self.buf):
            self.buf += d
            return
        self.flush()
        if len(d) <= self.max:
            self.buf += d
            return
        for i in range(0, len(d), self.max):
            self.packets.append(bytes(d[i:i + self.max]))
