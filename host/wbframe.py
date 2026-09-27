"""Link framing and control-line helpers, Python mirror of firmware/main/frame.c and proto.c.

Wire contract: docs/PROTOCOL.md. Standard library only (runs on the HeadUnit's stock Python 3.12).
"""

END, ESC, ESC_END, ESC_ESC = 0xC0, 0xDB, 0xDC, 0xDD
T_IP, T_CTL = 0x01, 0x02
MAX_PAYLOAD = 1500


def crc16(data, crc=0xFFFF):
    """CRC-16/CCITT-FALSE."""
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def encode(ftype, payload):
    body = bytes([ftype]) + bytes(payload)
    c = crc16(body)
    body += bytes([c >> 8, c & 0xFF])
    body = body.replace(b'\xdb', b'\xdb\xdd').replace(b'\xc0', b'\xdb\xdc')
    return b'\xc0' + body + b'\xc0'


class Decoder:
    """Incremental decoder; feed() returns a list of (type, payload) for frames with a valid CRC."""

    def __init__(self):
        self._buf = bytearray()
        self.frames_ok = self.crc_errors = self.discarded = 0

    def feed(self, data):
        out = []
        self._buf += data
        parts = self._buf.split(b'\xc0')
        self._buf = bytearray(parts.pop())  # unterminated tail
        for raw in parts:
            if not raw:
                continue
            body = _unescape(bytes(raw))
            if body is None or len(body) > MAX_PAYLOAD + 3:
                self.discarded += 1
            elif len(body) < 3 or crc16(body[:-2]) != (body[-2] << 8 | body[-1]):
                self.crc_errors += 1
            else:
                self.frames_ok += 1
                out.append((body[0], body[1:-2]))
        if len(self._buf) > 2 * (MAX_PAYLOAD + 3) + 16:  # garbage without delimiters
            self._buf.clear()
            self.discarded += 1
        return out


def _unescape(raw):
    if b'\xdb' not in raw:
        return raw
    out = bytearray()
    i, n = 0, len(raw)
    while i < n:
        c = raw[i]
        if c == ESC:
            if i + 1 >= n:
                return None
            nxt = raw[i + 1]
            if nxt == ESC_END:
                out.append(END)
            elif nxt == ESC_ESC:
                out.append(ESC)
            else:
                return None
            i += 2
        else:
            out.append(c)
            i += 1
    return bytes(out)


def pct_encode(data):
    """Percent-encode bytes (or a str as UTF-8): 0x00-0x20, '%', 0x7F-0xFF -> %XX."""
    if isinstance(data, str):
        data = data.encode('utf-8')
    return ''.join('%%%02X' % b if (b <= 0x20 or b == 0x25 or b >= 0x7F) else chr(b) for b in data)


def pct_decode(text):
    """Inverse of pct_encode; returns bytes. Raises ValueError on malformed input."""
    out = bytearray()
    i = 0
    while i < len(text):
        ch = text[i]
        if ch == '%':
            h = text[i + 1:i + 3]
            if len(h) != 2 or any(c not in '0123456789abcdefABCDEF' for c in h):
                raise ValueError('bad percent escape at %d' % i)
            out.append(int(h, 16))
            i += 3
        else:
            out += ch.encode('utf-8')
            i += 1
    return bytes(out)


def parse_fields(tokens):
    """['k=v', ...] -> {k: v} with values left encoded."""
    d = {}
    for t in tokens:
        k, sep, v = t.partition('=')
        if sep:
            d[k] = v
    return d
