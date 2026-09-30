#!/usr/bin/env python3
"""Machine checks for the wire formats: Python mirror self-tests, then the firmware's portable C
(frame.c, proto.c, sbp.c) compiled on the host and compared byte-for-byte with host/wbframe.py and
host/sbpframe.py.

  python tests/test_all.py        # exit 0 = all PASS

C compiler: $CC, else gcc/cc/clang on PATH, else MSVC (vswhere). No compiler -> C part FAILS
(it is not silently skipped).
"""
import os
import random
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'host'))
import wbframe as W  # noqa: E402
import sbpframe as SB  # noqa: E402
import kframe as KF  # noqa: E402

FW = os.path.join(ROOT, 'firmware', 'main')
results = []
NL = chr(10)


def check(name, cond, detail=''):
    results.append((name, bool(cond)))
    print('%s  %s%s' % ('PASS' if cond else 'FAIL', name, ('  -- ' + detail) if (detail and not cond) else ''))


# ---------------------------------------------------------------- Python-only
def py_tests():
    rng = random.Random(1)
    frames = [(rng.choice([W.T_IP, W.T_CTL]), bytes(rng.choice([0xC0, 0xDB, rng.randrange(256)])
                                                  for _ in range(rng.randrange(0, 1501)))) for _ in range(200)]
    stream = b''.join(W.encode(t, p) for t, p in frames)
    d = W.Decoder()
    got = []
    i = 0
    while i < len(stream):  # random split points
        n = rng.randrange(1, 700)
        got += d.feed(stream[i:i + n])
        i += n
    check('py: 200 random frames round-trip with random splits', got == frames)

    d = W.Decoder()
    junk = b'ESP-ROM:esp32c3-api1-20210207' + bytes([13, 10]) + b'rst:0x1 (POWERON)' + bytes([13, 10])
    got = d.feed(junk + W.encode(W.T_CTL, b'* HELLO fw=0.1.0'))
    check('py: boot-log junk before a frame is rejected, frame survives',
          got == [(W.T_CTL, b'* HELLO fw=0.1.0')] and d.crc_errors == 1, str((got, d.crc_errors)))

    enc = bytearray(W.encode(W.T_IP, b'E' + bytes(30)))
    enc[5] ^= 0x01
    d = W.Decoder()
    check('py: single bit flip -> CRC error', d.feed(bytes(enc)) == [] and d.crc_errors == 1)

    d = W.Decoder()
    check('py: bad escape -> discarded', d.feed(bytes([0xC0, 0x01, 0xDB, 0x41, 0, 0, 0xC0])) == [] and d.discarded == 1)

    d = W.Decoder()
    big = W.encode(W.T_IP, bytes(1600))
    check('py: oversize frame discarded', d.feed(big) == [] and d.discarded == 1)

    for s in ['\u041b\u043e\u0434\u043a\u0430 Kogger 5G', 'a b%c', '', 'x' * 32, bytes([0, 0x7F, 0xFF]).decode('latin1')]:
        e = W.pct_encode(s)
        ok = ' ' not in e and all(0x21 <= ord(ch) <= 0x7E for ch in e) and W.pct_decode(e) == s.encode('utf-8')
        check('py: pct round-trip %s' % ascii(s), ok, e)
    try:
        W.pct_decode('%G1')
        check('py: pct rejects bad escape', False)
    except ValueError:
        check('py: pct rejects bad escape', True)

    # SBP mirror self-tests
    f = SB.encode(0, SB.mode(SB.SETTING, 0, resp=True), SB.ID_UART,
                  SB.KEY_CONFIRM.to_bytes(4, 'little') + bytes([1]) + (2000000).to_bytes(4, 'little'))
    check('py: SBP frame layout = KoggerApp setBaudrate(2000000)',
          f[:6] == bytes([0xBB, 0x55, 0x00, 0x82, 0x18, 9]) and len(f) == 17)
    d = SB.Decoder()
    got = d.feed(b'junk' + bytes([0xBB]) + f + bytes([0xBB, 0xBB, 0x55]) + f[2:])  # "BB BB 55" syncs on the 2nd BB
    check('py: SBP decoder resyncs on BB BB 55', len(got) == 2 and all(g.id == SB.ID_UART for g in got)
          and d.check_errors == 0, str(got))
    bad = bytearray(f)
    bad[8] ^= 1
    d = SB.Decoder()
    check('py: SBP bit flip -> checksum error', d.feed(bytes(bad)) == [] and d.check_errors == 1)
    check('py: X.25 (CRC-16/MCRF4XX) known answer: "123456789" -> 0x6F91', KF.x25(b'123456789') == 0x6F91,
          hex(KF.x25(b'123456789')))
    fr = KF.Framer(4096)
    hb = KF.mav1(0, bytes(9))
    fr.feed(b'noise' + hb + KF.mav1(0, bytes(9), extra=51) + KF.mav2(33, bytes(28), signed=True))
    fr.flush(force=True)
    kinds = [(k, fl) for k, _, fl in fr.out]
    check('py: MAVLink v1 frame found, wrong CRC_EXTRA rejected, signed v2 found',
          ('F', 4) in kinds and ('F', 5) in kinds and sum(1 for k in kinds if k[0] == 'F') == 2, kinds)
    p = SB.connect_payload('Boat-Network', 'pass1234')
    check('py: ID_WIFI v3 payload layout', p == bytes([1, 12]) + b'Boat-Network' + bytes([8]) + b'pass1234')


# ---------------------------------------------------------------- C harness
def find_compiler():
    cc = os.environ.get('CC')
    if cc:
        return ('gcc', cc)
    for c in ('gcc', 'cc', 'clang'):
        if shutil.which(c):
            return ('gcc', c)
    vswhere = os.path.join(os.environ.get('ProgramFiles(x86)', 'C:' + os.sep + 'Program Files (x86)'),
                           'Microsoft Visual Studio', 'Installer', 'vswhere.exe')
    if os.path.exists(vswhere):
        p = subprocess.run([vswhere, '-latest', '-products', '*', '-property', 'installationPath'],
                           stdout=subprocess.PIPE, text=True).stdout.strip()
        bat = os.path.join(p, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
        if os.path.exists(bat):
            return ('msvc', bat)
    return (None, None)


def build(tmp):
    kind, tool = find_compiler()
    srcs = [os.path.join(ROOT, 'tests', 'c_harness.c')] + [os.path.join(FW, f) for f in
                                                          ('frame.c', 'proto.c', 'sbp.c', 'kframe.c', 'kpack.c', 'mavcrc.c', 'portinfo.c',
                                                           'bootkey.c', 'survey.c', 'linkrate.c')]
    exe = os.path.join(tmp, 'c_harness.exe' if os.name == 'nt' else 'c_harness')
    if kind == 'gcc':
        cmd = [tool, '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror', '-I', FW, '-o', exe] + srcs
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    elif kind == 'msvc':
        bat = os.path.join(tmp, 'b.bat')
        with open(bat, 'w') as f:
            f.write('@call "%s" >nul' % tool + chr(13) + NL)
            # /Fo"dir\\": a single backslash before the quote would escape it for cl's parser
            f.write('cl /nologo /O2 /W4 /WX /wd4996 /I "%s" /Fe"%s" /Fo"%s%s%s" %s' % (
                FW, exe, tmp, os.sep, os.sep, ' '.join('"%s"' % s for s in srcs)) + chr(13) + NL)
        r = subprocess.run(['cmd', '/c', bat], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    else:
        check('c: compiler found', False, 'set CC or install gcc/MSVC')
        return None
    check('c: harness builds with warnings as errors (%s)' % kind, r.returncode == 0, r.stdout[-2000:])
    return exe if r.returncode == 0 else None


def lcg_vectors():
    state = 12345
    out = []
    for v, n in enumerate([0, 1, 2, 3, 7, 64, 255, 1000, 1500]):
        t = W.T_CTL if v % 2 else W.T_IP
        p = bytearray()
        for _ in range(n):
            state = (state * 1103515245 + 12345) & 0xFFFFFFFF
            r = (state >> 16) & 0xFF
            p.append(0xC0 if (r & 7) == 0 else 0xDB if (r & 7) == 1 else r)
        out.append((t, bytes(p)))
    return out


def c_tests(exe, tmp):
    def run(args, inp=None):
        return subprocess.run([exe] + args, input=inp, stdout=subprocess.PIPE, text=True).stdout

    lines = run(['enc']).split(NL)
    vec = lcg_vectors()
    ok = True
    for (t, p), line in zip(vec, lines):
        ct, cn, chex = line.split()
        if int(ct) != t or int(cn) != len(p) or bytes.fromhex(chex) != W.encode(t, p):
            ok = False
    check('c: frame_encode == wbframe.encode on %d vectors (incl. 1500 B, C0/DB-heavy)' % len(vec), ok)

    rng = random.Random(7)
    stream = bytearray(b'ESP-ROM:esp32c3 junk' + bytes([13, 10]))
    expect = []
    for i in range(300):
        t = rng.choice([W.T_IP, W.T_CTL])
        p = bytes(rng.choice([0xC0, 0xDB, rng.randrange(256)]) for _ in range(rng.randrange(0, 1501)))
        e = bytearray(W.encode(t, p))
        kind = i % 10
        if kind == 3:
            e[len(e) // 2] ^= 0x20       # corrupt
        elif kind == 7:
            e[1:1] = bytes([0xDB, 0x41])  # invalid escape
        else:
            expect.append((t, p))
        stream += e
    stream += W.encode(W.T_IP, bytes(1700))  # oversize
    path = os.path.join(tmp, 'stream.bin')
    with open(path, 'wb') as f:
        f.write(stream)
    out = run(['dec', path]).strip().split(NL)
    cframes = [(int(l.split()[1]), bytes.fromhex(l.split()[2]) if len(l.split()) > 2 else b'')
               for l in out if l.startswith('F ')]
    cstats = [int(x) for x in out[-1].split()[1:]]
    pd = W.Decoder()
    pframes = pd.feed(bytes(stream))
    check('c: decoder output == expected frames (%d of 300 valid)' % len(expect), cframes == expect)
    check('c: decoder output == Python decoder', cframes == pframes)
    check('c: decoder stats == Python stats (ok/crc/discarded)',
          cstats == [pd.frames_ok, pd.crc_errors, pd.discarded], '%s vs %s' % (
              cstats, [pd.frames_ok, pd.crc_errors, pd.discarded]))

    samples = [b'', b'Kogger Boat', '\u041b\u043e\u0434\u043a\u0430 5G'.encode(), b'%%%', bytes(range(256)), b'a=b c']
    out = run(['pct'], ''.join(s.hex() + NL for s in samples)).split(NL)
    ok = True
    for i, s in enumerate(samples):
        e, dline = out[2 * i][2:], out[2 * i + 1][2:]
        if e != W.pct_encode(s) or bytes.fromhex(dline) != s:
            ok = False
    check('c: pct_encode == wbframe.pct_encode, pct_decode round-trips', ok)

    parse_in = NL.join(['c1.7 CONNECT ssid=Kogger%20Boat pass=12345678', '  9   STATUS  ', 'only',
                        'x SCAN a b c d e f g h i j k l m']) + NL
    out = run(['parse'], parse_in).split(NL)
    check('c: proto_parse tag/cmd/args + proto_arg',
          out[0] == 'P 0 c1.7 CONNECT 2 | ssid=Kogger%20Boat pass=12345678 | ssid=Kogger%20Boat'
          and out[1] == 'P 0 9 STATUS 0 | | ssid=<none>' and out[2] == 'P -1' and out[3] == 'P -1', repr(out[:4]))

    # ---- SBP
    lines = run(['sbpenc']).split(NL)
    st, ok, nvec = 777, True, 0
    for v, n in enumerate([0, 1, 2, 7, 64, 200, 255]):
        pl = bytearray()
        for _ in range(n):
            st = (st * 1103515245 + 12345) & 0xFFFFFFFF
            r = (st >> 16) & 0xFF
            pl.append(0xBB if (r & 7) == 0 else 0x55 if (r & 7) == 1 else r)
        route, mode_, id_ = (v * 37) & 0xFF, SB.mode(v % 3 + 1, v % 8, v & 1, (v >> 1) & 1), 0x57 ^ v
        cr, cm, ci, cl, ch = lines[v].split()
        if (int(cr), int(cm), int(ci), int(cl)) != (route, mode_, id_, n) or \
                bytes.fromhex(ch) != SB.encode(route, mode_, id_, pl):
            ok = False
        nvec += 1
    check('c: sbp_encode == sbpframe.encode on %d vectors (0..255 B, BB/55-heavy)' % nvec, ok)

    rng = random.Random(11)
    # A real "BB 55" in junk starts a bogus frame whose length byte swallows what follows (KoggerApp's
    # parser does the same), so the junk here has a lone BB only; both decoders are compared below anyway.
    stream = bytearray(bytes([0, 0xBB, 0x00]) + b' ESP-ROM junk ' + bytes([0xC0, 0xC0]))
    expect = []
    for i in range(400):
        pl = bytes(rng.choice([0xBB, 0x55, rng.randrange(256)]) for _ in range(rng.randrange(0, 256)))
        route, mode_, id_ = rng.randrange(256), rng.randrange(256), rng.randrange(256)
        e = bytearray(SB.encode(route, mode_, id_, pl))
        if i % 9 == 4:
            e[-1] ^= 0x5A                  # corrupt checksum
        else:
            expect.append((route, mode_, id_, pl))
        if i % 13 == 6:
            e[0:0] = bytes([0xBB])         # BB BB 55
        stream += e
    path = os.path.join(tmp, 'sbp.bin')
    with open(path, 'wb') as fh:
        fh.write(stream)
    out = run(['sbpdec', path]).strip().split(NL)
    cfr = []
    for l in out:
        if l.startswith('F '):
            parts = l.split()
            cfr.append((int(parts[1]), int(parts[2]), int(parts[3]), bytes.fromhex(parts[6]) if len(parts) > 6 else b''))
    cst = [int(x) for x in out[-1].split()[1:]]
    pd = SB.Decoder()
    pfr = [(f.route, f.mode, f.id, f.payload) for f in pd.feed(bytes(stream))]
    check('c: SBP decoder == expected frames (%d of 400 valid)' % len(expect), cfr == expect)
    check('c: SBP decoder == Python decoder, stats equal', cfr == pfr and cst == [pd.frames_ok, pd.check_errors],
          '%s vs %s' % (cst, [pd.frames_ok, pd.check_errors]))

    relay_tests(run, tmp)


# ---------------------------------------------------------------- relay framing (kframe + kpack)
RELAY_CAP = 4096


def rnd_bytes(rng, n, avoid=()):
    out = bytearray()
    while len(out) < n:
        b = rng.choice([0xBB, 0xCC, 0x55, rng.randrange(256)])
        if b not in avoid:
            out.append(b)
    return bytes(out)


MAV_IDS = sorted(KF.MAV)


def relay_stream(rng, hostile):
    """Returns (stream, valid frames <= RELAY_CAP in order, count of frames longer than 512)."""
    stream, frames = bytearray(), []
    for _ in range(900):
        r = rng.random()
        if r < 0.28:
            f = KF.kp1(rng.randrange(256), rng.randrange(256), rng.randrange(256), rnd_bytes(rng, rng.randrange(256)))
        elif r < 0.36:
            f = KF.ubx(rng.randrange(256), rng.randrange(256), rnd_bytes(rng, rng.choice([0, 20, 100, 600, 1500])))
        elif r < 0.44:
            mid = rng.choice([i for i in MAV_IDS if i < 256])
            f = KF.mav1(mid, rnd_bytes(rng, KF.MAV[mid][1]), rng.randrange(256), rng.randrange(256), rng.randrange(256))
        elif r < 0.52:
            mid = rng.choice(MAV_IDS)
            f = KF.mav2(mid, rnd_bytes(rng, rng.randrange(1, KF.MAV[mid][2] + 1)), rng.randrange(256),
                        signed=rng.random() < 0.3)
        elif r < 0.55:  # look like MAVLink but are not: wrong CRC_EXTRA, wrong v1 length, unknown id
            mid = rng.choice([i for i in MAV_IDS if i < 256])
            bad = rng.choice([KF.mav1(mid, rnd_bytes(rng, KF.MAV[mid][1]), extra=(KF.MAV[mid][0] + 1) & 0xFF),
                              KF.mav1(mid, rnd_bytes(rng, KF.MAV[mid][1] - 1 if KF.MAV[mid][1] > 1 else 2)),
                              KF.mav2(mid, rnd_bytes(rng, 4), extra=(KF.MAV[mid][0] + 7) & 0xFF)])
            stream += bad
            continue
        elif r < 0.62:
            n = rng.choice([rng.randrange(1, 300), rng.randrange(300, 1200), rng.randrange(1200, RELAY_CAP - 6 + 1)])
            f = KF.kp2(rnd_bytes(rng, n))
        elif r < 0.65:  # announces more than the buffer takes: must come through as raw, not lost
            stream += KF.kp2(rnd_bytes(rng, RELAY_CAP + rng.randrange(1, 500)))
            continue
        elif r < 0.85:  # noise; the clean stream has no sync bytes in it
            stream += rnd_bytes(rng, rng.randrange(1, 60), () if hostile else (0xBB, 0xCC, 0xB5, 0xFE, 0xFD))
            continue
        else:           # corrupted KP1 frame
            f = bytearray(KF.kp1(1, 2, 3, rnd_bytes(rng, rng.randrange(1, 100))))
            f[rng.randrange(2, len(f))] ^= 0x10
            stream += f
            continue
        stream += f
        frames.append(bytes(f))
    return bytes(stream), frames, sum(1 for f in frames if len(f) > 512)


def py_relay(stream, cap):
    """Same feeding/flush schedule as `c_harness relay`: 41-byte chunks, soft idle every 24 chunks,
    forced release at the end."""
    fr, pk = KF.Framer(cap), KF.Packer(512)
    units, done = [], 0

    def drain():
        nonlocal done
        for kind, d, fl in fr.out[done:]:
            units.append((kind, d, fl))
            pk.unit(d)
        done = len(fr.out)

    for i in range(0, len(stream), 41):
        fr.feed(stream[i:i + 41])
        drain()
        if (i // 41 + 1) % 24 == 0:
            fr.flush()
            drain()
            pk.flush()
    fr.flush(force=True)
    drain()
    pk.flush()
    return units, pk.packets


def relay_checks(tag, stream, units, packets):
    check('%s: units concatenate to the input (nothing lost, order kept)' % tag, b''.join(u[1] for u in units) == stream)
    check('%s: packets concatenate to the input' % tag, b''.join(packets) == stream)
    check('%s: every packet <= 512 bytes (%d packets)' % (tag, len(packets)), all(len(p) <= 512 for p in packets))
    # whole units up to 512 bytes never straddle a packet boundary
    bounds, pos = set(), 0
    for p in packets:
        pos += len(p)
        bounds.add(pos)
    pos, ok = 0, True
    for kind, d, _ in units:
        if len(d) <= 512:
            if any(pos < b < pos + len(d) for b in range(pos + 1, pos + len(d)) if b in bounds):
                ok = False
        pos += len(d)
    check('%s: no whole frame/raw run <= 512 B is split across packets' % tag, ok)


def adversarial_test(run, tmp):
    """False UBX syncs every 6 bytes, each announcing ~4 KB: without the rescan budget this costs
    ~4 KB of work per 6 input bytes. Both implementations must stay linear and agree."""
    stream = b'\xb5\x62\x00\x00\xf0\x0f' * 10000 + KF.kp1(1, 2, 3, b'tail')
    path = os.path.join(tmp, 'relay_adv.bin')
    with open(path, 'wb') as fh:
        fh.write(stream)
    t0 = time.monotonic()
    out = run(['relay', path, str(RELAY_CAP)]).strip().split(NL)
    tc = time.monotonic() - t0
    kinds = {1: 'F', 2: 'R'}
    cunits = []
    for l in out:
        parts = l.split()
        if l.startswith('U '):
            cunits.append((kinds[int(parts[1])], bytes.fromhex(parts[3]) if len(parts) > 3 else b'', int(parts[2])))
    t0 = time.monotonic()
    punits, _ = py_relay(stream, RELAY_CAP)
    tp = time.monotonic() - t0
    check('relay adversarial: 60 KB of false syncs, C == Python, linear (C %.2f s, Python %.2f s)' % (tc, tp),
          cunits == punits and b''.join(u[1] for u in cunits) == stream and tp < 20)


def relay_tests(run, tmp):
    adversarial_test(run, tmp)
    for tag, hostile, seed in (('relay clean', False, 21), ('relay hostile', True, 22)):
        rng = random.Random(seed)
        stream, frames, nbig = relay_stream(rng, hostile)
        path = os.path.join(tmp, 'relay_%d.bin' % seed)
        with open(path, 'wb') as fh:
            fh.write(stream)
        out = run(['relay', path, str(RELAY_CAP)]).strip().split(NL)
        kinds = {1: 'F', 2: 'R'}
        cunits, cpackets = [], []
        for l in out:
            parts = l.split()
            if l.startswith('U '):
                cunits.append((kinds[int(parts[1])], bytes.fromhex(parts[3]) if len(parts) > 3 else b'', int(parts[2])))
            elif l.startswith('P '):
                cpackets.append(bytes.fromhex(parts[1]))
        punits, ppackets = py_relay(stream, RELAY_CAP)
        check('%s: C units == Python units (%d)' % (tag, len(cunits)), cunits == punits)
        check('%s: C packets == Python packets' % tag, cpackets == ppackets)
        relay_checks(tag, stream, cunits, cpackets)
        if not hostile:
            found = [d for k, d, _ in cunits if k == 'F']
            check('%s: all %d valid frames found whole, in order (%d of them > 512 B)' % (tag, len(frames), nbig),
                  found == frames)
            protos = {}
            for k, d, fl in cunits:
                if k == 'F':
                    protos[fl] = protos.get(fl, 0) + 1
            names = {1: 'KP1', 2: 'KP2', 3: 'UBX', 4: 'MAV1', 5: 'MAV2'}
            check('%s: every protocol recognised: %s' % (tag, ', '.join('%s %d' % (names[p], n) for p, n in sorted(protos.items()))),
                  sorted(protos) == [1, 2, 3, 4, 5])
            # a frame longer than 512 goes out as consecutive 512-byte pieces that add up to it
            ok, pos, starts = True, 0, {}
            for i, pkt in enumerate(cpackets):
                starts[pos] = i
                pos += len(pkt)
            upos = 0
            for k, d, _ in cunits:
                if k == 'F' and len(d) > 512:
                    i = starts.get(upos)
                    pieces = []
                    while i is not None and sum(map(len, pieces)) < len(d):
                        pieces.append(cpackets[i])
                        i += 1
                    if b''.join(pieces) != d or any(len(x) != 512 for x in pieces[:-1]):
                        ok = False
                upos += len(d)
            check('%s: frames > 512 B cut into 512-byte packets that add up to the frame' % tag, ok)


def link_report_tests():
    """ID_WIFI v1 by firmware generation, read with host/sbpframe.parse_link."""
    base = bytes([3, 0xC4]) + bytes(20)
    check('py: ID_WIFI v1 of 0.10 (22 B): no reset reason, no drop count',
          'reset' not in SB.parse_link(base) and 'req_drops' not in SB.parse_link(base))
    d = SB.parse_link(base + bytes([3]))
    check('py: ID_WIFI v1 of 0.11 (23 B): reset reason SW', d.get('reset') == 'SW' and 'req_drops' not in d)
    d = SB.parse_link(base + bytes([3]) + (7).to_bytes(4, 'little'))
    check('py: ID_WIFI v1 of 0.14 (27 B): reset reason and 7 dropped requests', d.get('reset') == 'SW' and d.get('req_drops') == 7)


def bootkey_tests(exe):
    """BOOT button poll step (firmware/main/bootkey.c), 50 ms per poll: 100 polls down = 5 s, 200 = 10 s."""
    def run(seq):
        return subprocess.run([exe, 'bootkey', seq], stdout=subprocess.PIPE, text=True).stdout.strip()

    out = run('u' + 'd' * 99 + 'uu')
    check('bootkey: 4.95 s press does nothing, LED untouched', out == '.' * 102, out[-5:])
    out = run('u' + 'd' * 100 + 'uu')
    check('bootkey: 5 s press arms the rate reset (LED on) and asks for it on release', out == '.' * 100 + '**r', out[-5:])
    out = run('u' + 'd' * 199 + 'uu')
    check('bootkey: 9.95 s press is still the rate reset', out.endswith('r') and 'R' not in out, out[-5:])
    out = run('u' + 'd' * 200 + 'uu')
    check('bootkey: 10 s press arms the full reset and asks for it on release', out.endswith('**R') and 'r' not in out,
          out[-5:])
    out = run('u' + 'd' * 120)
    check('bootkey: rate reset armed: LED 250 ms on / 250 ms off', out.endswith('*' * 5 + '-' * 5 + '*' * 5 + '-' * 5 + '*'),
          out[-21:])
    out = run('u' + 'd' * 210)
    check('bootkey: full reset armed: LED 100 ms on / 100 ms off', out.endswith('**--**--**-'), out[-11:])
    out = run('u' + 'd' * 150 + 'u' + 'd' * 10 + 'uu')
    check('bootkey: one released poll while armed is bounce, not a release', out.count('r') == 1 and out.endswith('r'))
    out = run('u' + 'd' * 60 + 'u' + 'd' * 40 + 'uu')
    check('bootkey: a bounce before 5 s does not restart the count', out.endswith('**r'), out[-5:])
    out = run('u' + 'd' * 90 + 'uu' + 'd' * 90 + 'uu')
    check('bootkey: a real release restarts the count', out == '.' * len(out))
    out = run('d' * 300 + 'uu')
    check('bootkey: down since polling started is ignored', out == '.' * len(out))
    out = run('d' * 50 + 'u' + 'd' * 200 + 'uu')
    check('bootkey: after that a real press works', out.endswith('**R'), out[-5:])


def linkrate_tests(exe):
    """Link rate decoding, counting and pages (firmware/main/linkrate.c), read with host/sbpframe.parse_linkrate."""
    def run(script):
        return subprocess.run([exe, 'linkrate'], input=NL.join(script) + NL, stdout=subprocess.PIPE,
                              text=True).stdout.split(NL)

    def kbps(kind, code, flags=0):
        return int(run(['kbps %d %d %d' % (kind, code, flags)])[0].split()[1])

    check('linkrate: 11b 1 Mbit/s long preamble', kbps(1, 0x00) == 1000, str(kbps(1, 0x00)))
    check('linkrate: 11b 11 Mbit/s short preamble', kbps(1, 0x07) == 11000, str(kbps(1, 0x07)))
    check('linkrate: code 0x04 is no 11b rate', kbps(1, 0x04) == 0, str(kbps(1, 0x04)))
    check('linkrate: 11g 6 and 54 Mbit/s', (kbps(2, 0x0B), kbps(2, 0x0C)) == (6000, 54000),
          str((kbps(2, 0x0B), kbps(2, 0x0C))))
    check('linkrate: 11n MCS7 20 MHz 65.0 / short GI 72.2 Mbit/s', (kbps(3, 7), kbps(3, 7, 1)) == (65000, 72200),
          str((kbps(3, 7), kbps(3, 7, 1))))
    check('linkrate: 11n MCS0 40 MHz short GI 15 Mbit/s, MCS7 150 Mbit/s', (kbps(3, 0, 3), kbps(3, 7, 3)) ==
          (15000, 150000), str((kbps(3, 0, 3), kbps(3, 7, 3))))
    check('linkrate: a second spatial stream (MCS8) is not in the one-stream table', kbps(3, 8) == 0)
    check('linkrate: an LR rate is not known (no documented encoding)', kbps(4, 0x0A) == 0)

    def cls(*a):
        return tuple(int(x) for x in run(['class %d %d %d %d %d %d' % a])[0].split()[1:])

    check('linkrate: an HT frame is kind ht, code = MCS, flags = short GI + 40 MHz', cls(1, 0, 5, 1, 1, 0) == (3, 5, 3),
          str(cls(1, 0, 5, 1, 1, 0)))
    check('linkrate: a non-HT frame of an LR link is kind lr with its raw rate field', cls(0, 0x0A, 0, 0, 0, 1) ==
          (4, 0x0A, 0), str(cls(0, 0x0A, 0, 0, 0, 1)))
    check('linkrate: non-HT codes up to 0x07 are 11b, above are 11g', (cls(0, 3, 0, 0, 0, 0)[0],
          cls(0, 0x0C, 0, 0, 0, 0)[0]) == (1, 2))
    check('linkrate: the window 0 -> 200, 10 -> 50, 5000 -> 1000 ms', [int(run(['clamp %d' % w])[0].split()[1])
          for w in (0, 10, 5000)] == [200, 50, 1000])

    a, b, x = 'aabbccddeeff', '112233445566', '999999999999'
    script = ['peer %s 0' % a, 'peer %s 1' % b, 'begin 200 4',
              'frame %s -40 1 0 7 0 1' % a, 'frame %s -44 1 0 7 0 1' % a, 'frame %s -42 1 0 5 0 0' % a,
              'frame %s -60 0 10 0 0 0' % b, 'frame %s -30 1 0 7 0 1' % x, 'pages']
    out = run(script)
    pages = [SB.parse_linkrate(bytes.fromhex(l.split()[1])) for l in out if l.startswith('P ')]
    check('linkrate: one page per peer, a stranger ignored', len(pages) == 2 and pages[0]['total'] == 2, str(out))
    p = pages[0]
    check('linkrate: the peer, the window, the frames and the average RSSI', (p['peer'], p['window_ms'], p['frames'],
          p['rssi'], p['phy']) == ('aa:bb:cc:dd:ee:ff', 200, 3, -42, 'HT20'), str(p))
    check('linkrate: the rate with the most frames first (MCS7 short GI: 2, then MCS5: 1)',
          [(r['kind'], r['code'], r['sgi'], r['kbps'], r['frames']) for r in p['rates']] ==
          [('ht', 7, True, 72200, 2), ('ht', 5, False, 52000, 1)], str(p['rates']))
    q = pages[1]
    check('linkrate: a peer whose link may run LR: raw code, rate 0 = not known',
          [(r['kind'], r['code'], r['kbps']) for r in q['rates']] == [('lr', 10, 0)] and q['rssi'] == -60, str(q))

    many = ['peer %s 0' % a, 'begin 100 255'] + ['frame %s -50 1 0 %d 0 0' % (a, m % 8) for m in range(9)] + \
           ['frame %s -50 0 %d 0 0 0' % (a, 0x0B), 'pages']
    p = [SB.parse_linkrate(bytes.fromhex(l.split()[1])) for l in run(many) if l.startswith('P ')][0]
    check('linkrate: at most 4 rates reported, every frame counted', len(p['rates']) == 4 and p['frames'] == 10
          and p['phy'] == 'none', str(p))
    out = run(['begin 200 4', 'pages'])
    check('linkrate: no peer, no page', out[0] == 'N 0' and not [l for l in out if l.startswith('P ')], str(out))
    check('linkrate: the {0, 0} answer parses as no peer', SB.parse_linkrate(bytes([0, 0])) == dict(index=0, total=0))


def survey_tests(exe):
    """Channel survey arithmetic and pages (firmware/main/survey.c), read with host/sbpframe.parse_survey."""
    def run(script):
        return subprocess.run([exe, 'survey'], input=NL.join(script) + NL, stdout=subprocess.PIPE,
                              text=True).stdout.split(NL)

    def air(sig_mode, rate, mcs=0, sgi=0, length=100):
        out = run(['air %d %d %d %d %d' % (sig_mode, rate, mcs, sgi, length)])
        return int(out[0].split()[1])

    # 11b at 1 Mbit/s: 192 us of long preamble + 8 bits per byte at 1 bit/us
    check('survey: 11b 1 Mbit/s, 100 B -> 192 + 800 us', air(0, 0x00) == 992, str(air(0, 0x00)))
    check('survey: 11b 11 Mbit/s short preamble, 100 B -> 96 + 73 us', air(0, 0x07) == 169, str(air(0, 0x07)))
    # 11g 6 Mbit/s: 20 us + ceil((22 + 800) / 24) = 35 symbols of 4 us
    check('survey: 11g 6 Mbit/s, 100 B -> 20 + 140 us', air(0, 0x0B) == 160, str(air(0, 0x0B)))
    check('survey: 11g 54 Mbit/s is the shortest of the 11g rates', air(0, 0x0C) < air(0, 0x0B), str(air(0, 0x0C)))
    # 11n MCS0 20 MHz: 36 us + ceil(822 / 26) = 32 symbols, 4 us each (long guard interval)
    check('survey: 11n MCS0 long GI, 100 B -> 36 + 128 us', air(1, 0, 0, 0) == 164, str(air(1, 0, 0, 0)))
    check('survey: the same with a short guard interval is shorter', air(1, 0, 0, 1) == 36 + 116,
          str(air(1, 0, 0, 1)))
    check('survey: MCS7 carries 10x MCS0 per symbol', air(1, 0, 7, 0) < air(1, 0, 0, 0), str(air(1, 0, 7, 0)))
    check('survey: an unknown rate gives no air time', air(0, 0x04) == 0 and air(3, 0) == 0)

    out = run(['clamp 0 0', 'clamp 10 %d' % (1 << 12), 'clamp 5000 3'])
    got = [tuple(int(x) for x in l.split()[1:]) for l in out if l.startswith('C ')]
    check('survey: dwell 0 = default %d, mask 0 = channels 1..%d' % (SB.SURVEY_DWELL_DEF, SB.SURVEY_CH_SCAN_MAX),
          got[0] == (SB.SURVEY_DWELL_DEF, (1 << SB.SURVEY_CH_SCAN_MAX) - 1), str(got[0]))
    check('survey: a dwell under the minimum is pulled up, channel 13 is dropped (the country forbids it)',
          got[1] == (50, (1 << SB.SURVEY_CH_SCAN_MAX) - 1), str(got[1]))
    check('survey: a dwell over the maximum is pulled down, a mask of real channels is kept',
          got[2] == (1000, 3), str(got[2]))

    script = ['begin 7 100 6 001122334455', 'dwell 1 100', 'dwell 2 100', 'dwell 3 100',
              'frame 1 -40 -95 20000 aabbccddeeff 001122334455', 'frame 1 -60 -95 5000 aabbccddeeff ffffffffffff',
              'frame 1 -50 -90 5000 001122334455 001122334455', 'frame 2 -70 -95 100',
              'pages']
    out = run(script)
    pages = [SB.parse_survey(bytes.fromhex(l.split()[1])) for l in out if l.startswith('P ')]
    check('survey: 3 measured channels, one page each', len(pages) == 3 and pages[0]['total'] == 3, str(len(pages)))
    p1 = pages[0]
    check('survey: channel 1 busy 300 per mille (30 ms of air in 100 ms)', p1['busy_permille'] == 300, str(p1))
    check('survey: 3 frames, 2 transmitters, strongest RSSI -40', p1['frames'] == 3 and p1['senders'] == 2
          and p1['rssi'] == -40, str(p1))
    check('survey: the noise floor is the average of the samples', p1['noise'] == -93, str(p1['noise']))
    check('survey: channel 2 heard one frame, channel 3 nothing',
          pages[1]['frames'] == 1 and pages[2]['frames'] == 0 and pages[2]['rssi'] == -128, str(pages[2]))
    check('survey: the module own channel is flagged, and 1..11 as usable by an access point',
          not p1['flags']['home'] and p1['flags']['ap_ok'] and pages[2]['flags']['home'] is False, str(p1['flags']))
    check('survey: busy never exceeds 1000 per mille',
          all(0 <= p['busy_permille'] <= 1000 for p in pages))

    out = run(['begin 0 100 1', 'dwell 1 100', 'frame 1 -40 -95 1000 aabbccddeeff', 'pages'])
    p = [SB.parse_survey(bytes.fromhex(l.split()[1])) for l in out if l.startswith('P ')][0]
    check('survey: one unchanging noise figure is flagged as not to be trusted', p['flags']['noise_bad'], str(p))
    check('survey: the air time of the module own network is reported apart (0.17)',
          p1['own_permille'] == 250 and p1['busy_permille'] == 300, str((p1['own_permille'], p1['busy_permille'])))
    check('survey: a channel with none of the module own traffic reports 0 of it',
          pages[1]['own_permille'] == 0, str(pages[1]))

    out = run(['begin 1 100 1 001122334455', 'dwell 1 100',
               'frame 1 -40 -95 50000 aabbccddeeff 001122334455',
               'frame 1 -40 -95 90000 aabbccddeeff 001122334455', 'pages'])
    p = [SB.parse_survey(bytes.fromhex(l.split()[1])) for l in out if l.startswith('P ')][0]
    check('survey: the own share never exceeds the busy figure it is part of',
          p['own_permille'] <= p['busy_permille'] == 1000, str(p))
    check('survey: the module own channel is flagged', p['flags']['home'], str(p['flags']))


def portinfo_tests(exe):
    """ID_WIFI_NET v7 pages built by firmware/main/portinfo.c, read with host/sbpframe.parse_port."""
    def run(script):
        out = subprocess.run([exe, 'portinfo'], input=NL.join(script) + NL, stdout=subprocess.PIPE, text=True).stdout
        return [SB.parse_port(bytes.fromhex(l.split()[1])) for l in out.split(NL) if l.startswith('P')]

    host_req = KF.kp1(0, SB.mode(SB.GETTING, 2), SB.ID_VERSION, b'')          # discovery from a host
    host_set = KF.kp1(87, SB.mode(SB.SETTING, 0), SB.ID_UART, bytes(9))
    v2 = bytes([0, 0, 12, 0, 0, 0, 0, 10, 1])                                  # board 12, firmware 1.10
    v0 = bytes([0, 12]) + bytes(12) + bytes([4, 3, 2, 1]) + bytes(16)          # serial 0x01020304
    dev_v2 = KF.kp1(0, SB.mode(SB.CONTENT, 2), SB.ID_VERSION, v2)
    dev_v0 = KF.kp1(0, SB.mode(SB.CONTENT, 0), SB.ID_VERSION, v0)
    ack = KF.kp1(0, SB.mode(SB.CONTENT, 2, resp=True), SB.ID_VERSION, bytes([1, 2, 3]))  # an ack is no version
    hb = KF.mav2(0, bytes([0, 0, 0, 0, 10, 3, 81, 4]), sysid=1, compid=1)       # HEARTBEAT, version byte trimmed
    ubx = KF.ubx(0x01, 0x07, bytes(92))
    noise = bytes([0x31, 0x32, 0x41, 0x0d, 0x0a] * 6)
    net_req = KF.kp1(0, SB.mode(SB.GETTING, 0), 0x1C, b'')
    script = ['clock 10',
              'up 0 ' + (host_req + host_set).hex(),
              'up 1 ' + (dev_v2 + dev_v0 + ack).hex(),
              'clock 20',
              'up 1 ' + (hb + ubx + noise).hex(),
              'down 1 ' + (net_req + dev_v2).hex(),
              'rx 1 1000', 'tx 1 500', 'mrx 0', 'mtx 0', 'mtx 0',
              'clock 50',
              'p0 0 13 0', 'p0 1 3 0', 'p1 1', 'p0 0 0 1',
              'clock 200', 'p0 1 3 0',
              'up 0 ' + noise.hex(), 'clock 205', 'p0 0 1 0',
              'p1 0']
    p = run(script)
    ok = len(p) == 7
    check('portinfo: 7 pages from the script', ok, str(len(p)))
    if not ok:
        return
    x1, x2, dev, x1slip, x2late, x1noise, x1dev = p
    check('portinfo p0 X1: host (2 requests), flags/rates/overflows as given, module 1 in / 2 out',
          x1['connected'] == 'sbp_host' and x1['sbp_req_up'] == 2 and x1['sbp_content_up'] == 0 and
          x1['flags']['uart'] and x1['flags']['asked_here'] and x1['flags']['reports'] and not x1['flags']['slip'] and
          x1['baud'] == 921600 and x1['saved'] == 115200 and x1['rx_overflows'] == 7 and x1['tx_drops'] == 9 and
          x1['module_rx'] == 1 and x1['module_tx'] == 2 and x1['protos']['kp1']['up'] == 2 and
          x1['protos']['kp1']['age'] == 4.0 and x1['rx_age'] is None, str(x1))
    pr = x2['protos']
    check('portinfo p0 X2: devices win over MAVLink/u-blox; units by protocol both ways; bytes and ages',
          x2['connected'] == 'sbp_devices' and pr['kp1']['up'] == 3 and pr['kp1']['down'] == 2 and
          pr['mav2']['up'] == 1 and pr['ubx']['up'] == 1 and pr['raw']['up'] >= 1 and pr['kp1']['age'] == 4.0 and
          pr['mav2']['age'] == 3.0 and pr['kp2']['age'] is None and x2['sbp_content_up'] == 3 and
          x2['sbp_req_down'] == 1 and x2['sbp_content_down'] == 1 and x2['rx_bytes'] == 1000 and
          x2['tx_bytes'] == 500 and x2['rx_age'] == 3.0, str(x2))
    d = dev['devices']
    check('portinfo p1 X2: MAVLink system (heartbeat: type 10, autopilot 3) first, then SBP board 12 fw 1.10 '
          'serial 0x01020304; the ack is not a version',
          len(d) == 2 and d[0]['kind'] == 'mavlink' and d[0]['sysid'] == 1 and d[0]['compid'] == 1 and
          d[0]['mav_type'] == 10 and d[0]['autopilot'] == 3 and d[0]['heartbeat'] and d[0]['frames'] == 1 and
          d[1]['kind'] == 'sbp' and d[1]['addr'] == 0 and d[1]['board'] == 12 and d[1]['fw'] == (1, 10) and
          d[1]['version_known'] and d[1]['serial'] == 0x01020304 and d[1]['age'] == 4.0, str(d))
    check('portinfo p0 X1 on an IP bridge: slip only while bytes arrived within 10 s (none here: host stays)',
          x1slip['connected'] == 'sbp_host' and x1slip['flags'] == {n: False for n in SB.PORT_FLAGS}, str(x1slip))
    check('portinfo p0 X2 after 18 s of silence: nothing connected, ages grow',
          x2late['connected'] == 'nothing' and x2late['protos']['kp1']['age'] == 19.0 and x2late['rx_age'] == 18.0,
          str(x2late))
    check('portinfo p0 X1 with only unframed bytes lately: unreadable (another rate or protocol)',
          x1noise['connected'] == 'unreadable' and x1noise['protos']['raw']['up'] >= 1, str(x1noise))
    check('portinfo p1 X1: a host only, no devices', x1dev['devices'] == [], str(x1dev))
    check('py: discovery = GETTING ID_VERSION to 0/255 only',
          SB.is_discovery(0, SB.mode(SB.GETTING, 2), SB.ID_VERSION) and SB.is_discovery(255, SB.mode(SB.GETTING, 0), SB.ID_VERSION)
          and not SB.is_discovery(0, SB.mode(SB.SETTING, 0), SB.ID_VERSION) and not SB.is_discovery(0, SB.mode(SB.GETTING, 0), SB.ID_UART)
          and not SB.is_discovery(87, SB.mode(SB.GETTING, 0), SB.ID_VERSION))


def main():
    py_tests()
    link_report_tests()
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)
        if exe:
            c_tests(exe, tmp)
            portinfo_tests(exe)
            survey_tests(exe)
            linkrate_tests(exe)
            bootkey_tests(exe)
    failed = [n for n, ok in results if not ok]
    print(NL + '%d checks, %d failed' % (len(results), len(failed)))
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
