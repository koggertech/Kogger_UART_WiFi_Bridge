#!/usr/bin/env python3
"""Machine checks for the line 1 GODIO F9 -> MAVLink translator (firmware/main/f9.c, f9mav.c; docs/GODIO_F9.md).

  python tests/test_f9.py        # exit 0 = all PASS

- host/f9.py: the reference frame rule on the boat's F9 C4 and F8 frames; its stream form against the reference
  on random streams cut at random points;
- the C (f9.c, f9mav.c, mavcrc.c compiled on the PC) against host/f9.py, byte for byte: the frames found, the
  counters, and the MAVLink frames built from F9 C4, incl. odd values (negative, > 180, 360, NaN, no position);
- the MAVLink built from the boat's frame decoded by pymavlink (an independent implementation: field layout,
  CRC_EXTRA) and by the relay's framer (host/kframe.py: the relay would send every frame whole).
Compiler as tests/test_all.py ($CC, gcc/cc/clang, MSVC). pymavlink is optional (without it those checks are
skipped with a NOTE): python -m pip install pymavlink.
"""
import math
import os
import random
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'host'))
sys.path.insert(0, os.path.join(ROOT, 'tests'))
import f9 as F9  # noqa: E402
import kframe as KF  # noqa: E402
import test_all as TA  # noqa: E402  (compiler lookup and check() bookkeeping)

check = TA.check


def stream_case(rng):
    """Boat frames, F8 commands, noise with false syncs, broken frames - glued together."""
    parts = []
    for _ in range(rng.randrange(1, 30)):
        r = rng.random()
        if r < 0.35:
            parts.append(F9.REAL_C4)
        elif r < 0.5:
            parts.append(F9.REAL_F8)
        elif r < 0.7:
            body = bytes([0xF9, rng.choice([0xC0, 0xC4, 0xC5])]) + bytes(rng.randrange(256) for _ in range(16))
            parts.append(body + bytes([F9.cksum(body)]))
        elif r < 0.85:
            parts.append(bytes(rng.choice([0xF8, 0xF9, rng.randrange(256)]) for _ in range(rng.randrange(1, 50))))
        else:
            bad = bytearray(F9.REAL_C4)
            bad[rng.randrange(2, 18)] ^= 1 << rng.randrange(8)
            parts.append(bytes(bad))
    return b''.join(parts)


def splits(rng, data):
    i, out = 0, []
    while i < len(data):
        n = rng.randrange(1, 60)
        out.append(data[i:i + n])
        i += n
    return out


def py_tests():
    d = F9.pos(F9.REAL_C4)
    check('f9 py: the boat frame F9 C4 decodes: lon 124.0199, lat 42.5181, 12.4 V, heading 30.0, 38 sats, sum 0x18',
          d and abs(d['lon'] - 124.0199) < 1e-3 and abs(d['lat'] - 42.5181) < 1e-3 and d['volt_dv'] == 124
          and d['heading'] == 30.0 and d['sats'] == 38 and d['dist'] == 0 and F9.cksum(F9.REAL_C4[:-1]) == 0x18, str(d))
    check('f9 py: the boat frame F9 C0 (sum 0x8D) gives heading 30.0; a C4 is no C0 and a C0 no C4',
          F9.status_heading(F9.REAL_C0) == 30.0 and F9.cksum(F9.REAL_C0[:-1]) == 0x8D
          and F9.status_heading(F9.REAL_C4) is None and F9.pos(F9.REAL_C0) is None)
    s = bytes.fromhex('0011') + F9.REAL_C4 + F9.REAL_F8 + F9.REAL_C4 + bytes.fromhex('f999')
    check('f9 py: reference finds 3 frames in noise and back to back (f9_decode.py self-test)',
          F9.frames_of(s) == [F9.REAL_C4, F9.REAL_F8, F9.REAL_C4], str(F9.frames_of(s)))
    rng = random.Random(7)
    ok = True
    for _ in range(300):
        data = stream_case(rng)
        st = F9.Stream()
        for part in splits(rng, data):
            st.feed(part)
        st.end()
        if st.frames != F9.frames_of(data):
            ok = False
            break
    check('f9 py: the stream rule fed in random pieces finds what the reference finds in the whole (300 streams)', ok)
    rng = random.Random(8)
    ok = True
    for _ in range(200):
        segs = [stream_case(rng) for _ in range(rng.randrange(1, 5))]
        st = F9.Stream()
        want = []
        for seg in segs:
            for part in splits(rng, seg):
                st.feed(part)
            st.end()
            want += F9.frames_of(seg)
        if st.frames != want:
            ok = False
            break
    check('f9 py: an end (line quiet) between segments = the reference on each segment (200 cases)', ok)


def build(tmp):
    kind, tool = TA.find_compiler()
    fw = os.path.join(ROOT, 'firmware', 'main')
    srcs = [os.path.join(ROOT, 'tests', 'f9_harness.c')] + [os.path.join(fw, n) for n in ('f9.c', 'f9mav.c', 'mavcrc.c')]
    exe = os.path.join(tmp, 'f9_harness.exe' if os.name == 'nt' else 'f9_harness')
    if kind == 'gcc':
        r = subprocess.run([tool, '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror', '-I', fw, '-o', exe]
                           + srcs + ['-lm'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    elif kind == 'msvc':
        bat = os.path.join(tmp, 'b.bat')
        with open(bat, 'w') as f:
            f.write('@call "%s" >nul\r\n' % tool)
            f.write('cl /nologo /O2 /W4 /WX /wd4996 /I "%s" /Fe"%s" /Fo"%s%s%s" %s\r\n' % (
                fw, exe, tmp, os.sep, os.sep, ' '.join('"%s"' % s for s in srcs)))
        r = subprocess.run(['cmd', '/c', bat], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    else:
        check('f9 c: compiler found', False, 'set CC or install gcc/MSVC')
        return None
    check('f9 c: harness builds with warnings as errors (%s)' % kind, r.returncode == 0, r.stdout[-2000:])
    return exe if r.returncode == 0 else None


def run(exe, lines):
    out = subprocess.run([exe], input='\n'.join(lines) + '\n', stdout=subprocess.PIPE, text=True).stdout
    return out.split('\n')


def c_tests(exe):
    rng = random.Random(9)
    ok, detail = True, ''
    for case in range(150):
        segs = [stream_case(rng) for _ in range(rng.randrange(1, 4))]
        lines, st = [], F9.Stream()
        for seg in segs:
            for part in splits(rng, seg):
                lines.append('S ' + part.hex())
                st.feed(part)
            lines.append('E')
            st.end()
        lines.append('C')
        out = run(exe, lines)
        got = [bytes.fromhex(x[2:]) for x in out if x.startswith('F ')]
        cnt = [x for x in out if x.startswith('N ')]
        if got != st.frames or cnt != ['N %d %d' % (len(st.frames), st.skipped)]:
            ok, detail = False, 'case %d: %d vs %d frames, %s vs skipped %d' % (case, len(got), len(st.frames), cnt, st.skipped)
            break
    check('f9 c: f9_feed/f9_end == host/f9.py Stream: frames and counters (150 streams, random pieces)', ok, detail)

    def c4(lon, lat, hdg, volt=124, sats=38, dist=0):
        body = bytes([0xF9, 0xC4]) + struct.pack('<ffHBfB', lon, lat, dist, volt, hdg, sats)
        return body + bytes([F9.cksum(body)])
    frames = [F9.REAL_C4, c4(-70.123456, -33.987654, 359.996), c4(10.0, 20.0, 360.0), c4(10.0, 20.0, 181.5),
              c4(10.0, 20.0, -45.0), c4(0.0, 0.0, 90.0), c4(200.0, 20.0, 90.0), c4(10.0, 20.0, float('nan')),
              c4(float('inf'), 20.0, 12.0), c4(124.0199, 42.5181, 0.0, volt=255, sats=0)]
    rng = random.Random(10)
    for _ in range(200):
        frames.append(c4(rng.uniform(-180, 180), rng.uniform(-90, 90), rng.uniform(-400, 800),
                         rng.randrange(256), rng.randrange(256), rng.randrange(65536)))
    lines, want, seq, now = [], [], 250, 1
    for f in frames:
        now += rng.randrange(1, 10 ** 12)
        lines.append('P %s %d %d' % (f.hex(), now, seq))
        b, seq = F9.position(F9.pos(f), now, seq)
        want.append((b, seq & 0xFF))
    lines += ['H 7', 'P %s 5 0' % F9.REAL_F8.hex()]
    out = [x for x in run(exe, lines) if x]
    got = [(bytes.fromhex(out[i][2:]), int(out[i + 1][2:])) for i in range(0, 2 * len(frames), 2)]
    first_bad = next((i for i in range(len(frames)) if got[i] != want[i]), None)
    check('f9 c: MAVLink from F9 C4 == host/f9.py byte for byte (%d frames incl. odd values, sequence wrap)' % len(frames),
          first_bad is None, 'frame %s: %s / %s' % (first_bad, got[first_bad][0].hex() if first_bad is not None else '',
                                                     want[first_bad][0].hex() if first_bad is not None else ''))
    hb, nxt = F9.heartbeat(7)
    check('f9 c: HEARTBEAT == host/f9.py; a frame that is no F9 C4 builds nothing',
          out[2 * len(frames)] == 'M ' + hb.hex() and out[2 * len(frames) + 1] == 'Q %d' % nxt
          and out[2 * len(frames) + 2] == 'M -', str(out[-4:]))

    def c0(hdg, volt=124, sats=38):
        body = bytes([0xF9, 0xC0, 0x00, 0x01]) + bytes(8) + struct.pack('<BfB', volt, hdg, sats)
        return body + bytes([F9.cksum(body)])
    c0s = [F9.REAL_C0] + [c0(h) for h in (0.0, 360.0, 359.996, 200.0, -45.0, 1e-9, float('nan'), float('inf'))]
    c0s += [c0(rng.uniform(-400, 800)) for _ in range(200)]
    lines, want, seq, now = [], [], 3, 7
    for f in c0s:
        now += rng.randrange(1, 10 ** 12)
        lines.append('A %s %d %d' % (f.hex(), now, seq))
        b, seq = F9.attitude(F9.status_heading(f), now, seq)
        want.append(('M ' + b.hex() if b else 'M -', 'Q %d' % (seq & 0xFF)))
    lines += ['A %s 5 0' % F9.REAL_C4.hex(), 'P %s 5 0' % F9.REAL_C0.hex()]
    out = [x for x in run(exe, lines) if x]
    got = [(out[2 * i], out[2 * i + 1]) for i in range(len(c0s))]
    first_bad = next((i for i in range(len(c0s)) if got[i] != want[i]), None)
    check('f9 c: ATTITUDE from F9 C0 == host/f9.py byte for byte (%d frames incl. 0, 360, NaN, inf)' % len(c0s),
          first_bad is None, 'frame %s: %s / %s' % (first_bad, got[first_bad] if first_bad is not None else '',
                                                     want[first_bad] if first_bad is not None else ''))
    check('f9 c: a C4 builds no ATTITUDE, a C0 builds no position',
          out[2 * len(c0s)] == 'M -' and out[2 * len(c0s) + 2] == 'M -', str(out[-4:]))

    fronts = [F9.REAL_C0, c0(30.0, sats=0), c0(30.0, sats=255), c0(30.0, sats=9), c0(30.0, sats=10)]
    for _ in range(50):
        body = bytes([0xF9, 0xC0]) + bytes(rng.randrange(256) for _ in range(10)) + struct.pack('<BfB', 124, 1.0,
                                                                                                    rng.randrange(256))
        fronts.append(body + bytes([F9.cksum(body)]))
    lines, want, seq = [], [], 200
    for f in fronts + [F9.REAL_C4]:
        lines.append('T %s %d' % (f.hex(), seq))
        b, seq = F9.c0_front(f, seq)
        want.append(('M ' + b.hex() if b else 'M -', 'Q %d' % (seq & 0xFF)))
    out = [x for x in run(exe, lines) if x]
    got = [(out[2 * i], out[2 * i + 1]) for i in range(len(want))]
    check('f9 c: STATUSTEXT with the C0 front == host/f9.py byte for byte (%d frames, 0..255 satellites); '
          'none for a C4' % len(fronts), got == want and want[-1][0] == 'M -',
          str(next(((g, w) for g, w in zip(got, want) if g != w), '')))


def meaning_tests():
    b, seq = F9.position(F9.pos(F9.REAL_C4), 123456789, 0)
    a, seq = F9.attitude(F9.status_heading(F9.REAL_C0), 123456789, seq)
    b += a
    hb, _ = F9.heartbeat(seq)
    fr = KF.Framer(4096)
    fr.feed(b + hb)
    fr.flush(force=True)
    kinds = [(k, fl) for k, _, fl in fr.out]
    check("f9: the relay's framer takes all 5 frames whole as MAVLink 2, nothing raw",
          kinds == [('F', KF.P_MAV2)] * 5 and b''.join(d for _, d, _ in fr.out) == b + hb, str(kinds))
    try:
        from pymavlink.dialects.v20 import common as mav
    except ImportError:
        print('NOTE  f9: pymavlink is not installed - the independent decode was not run '
              '(python -m pip install pymavlink)')
        return
    m = mav.MAVLink(None)
    m.robust_parsing = False
    msgs = {}
    for x in b + hb:
        r = m.parse_char(bytes([x]))
        if r:
            msgs[r.get_type()] = r
    gp, at, gr, bs, hbm = (msgs.get(k) for k in ('GLOBAL_POSITION_INT', 'ATTITUDE', 'GPS_RAW_INT', 'BATTERY_STATUS',
                                                  'HEARTBEAT'))
    check('f9: pymavlink decodes all 5 (CRC and CRC_EXTRA right)', all((gp, at, gr, bs, hbm)), str(sorted(msgs)))
    if not all((gp, at, gr, bs, hbm)):
        return
    lat = int(round(struct.unpack('<f', F9.REAL_C4[6:10])[0] * 1e7))
    lon = int(round(struct.unpack('<f', F9.REAL_C4[2:6])[0] * 1e7))
    check('f9: GLOBAL_POSITION_INT lat/lon = deg x 1e7 (42.5181, 124.0199), hdg 3000 cdeg, time 123456 ms',
          gp.lat == lat and gp.lon == lon and abs(gp.lat - 425181000) <= 500 and abs(gp.lon - 1240199000) <= 500
          and gp.hdg == 3000 and gp.time_boot_ms == 123456, str(gp))
    check('f9: the boat frame F9 C0 -> ATTITUDE yaw = 30 deg in rad (0.5236), roll = pitch = 0, time 123456 ms',
          abs(at.yaw - math.radians(30.0)) < 1e-6 and at.roll == 0 and at.pitch == 0 and at.time_boot_ms == 123456,
          str(at))
    check('f9: GPS_RAW_INT fix 3, 38 satellites, the same lat/lon, course unknown',
          gr.fix_type == 3 and gr.satellites_visible == 38 and gr.lat == lat and gr.lon == lon and gr.cog == 65535
          and gr.time_usec == 123456789, str(gr))
    check('f9: BATTERY_STATUS voltages[0] = 12400 mV, other cells unknown',
          bs.voltages[0] == 12400 and all(v == 65535 for v in bs.voltages[1:]) and bs.battery_remaining == -1, str(bs))
    check('f9: HEARTBEAT surface boat (11), autopilot generic (0), active (4), sysid/compid 1/1',
          hbm.type == 11 and hbm.autopilot == 0 and hbm.system_status == 4 and hbm.get_srcSystem() == 1
          and hbm.get_srcComponent() == 1, str(hbm))
    b2, _ = F9.position(dict(lon=0.0, lat=0.0, dist=0, volt_dv=120, heading=float('nan'), sats=3), 1000, 0)
    m2 = mav.MAVLink(None)
    got = [r.get_type() for r in (m2.parse_char(bytes([x])) for x in b2) if r]
    def kinds(p):
        mm = mav.MAVLink(None)
        return [r for r in (mm.parse_char(bytes([x])) for x in F9.position(p, 1000, 0)[0]) if r]
    frozen = dict(lon=114.4058762, lat=22.7584991, dist=0, volt_dv=124, heading=30.0, sats=1)  # field capture 2026-10-07
    got1 = kinds(frozen)
    got4 = kinds(dict(frozen, sats=4))
    check('f9: 1 satellite (the frozen position of the field capture) -> no GLOBAL_POSITION_INT, GPS_RAW_INT fix 1 '
          'with lat/lon 0; 4 satellites -> position and fix 3',
          [r.get_type() for r in got1] == ['GPS_RAW_INT', 'BATTERY_STATUS'] and got1[0].fix_type == 1
          and got1[0].lat == 0 and got1[0].lon == 0 and got1[0].satellites_visible == 1
          and [r.get_type() for r in got4] == ['GPS_RAW_INT', 'GLOBAL_POSITION_INT', 'BATTERY_STATUS']
          and got4[0].fix_type == 3, str((got1, got4)))
    t, _ = F9.c0_front(F9.REAL_C0, 9)
    mm = mav.MAVLink(None)
    st = [r for r in (mm.parse_char(bytes([x])) for x in t) if r]
    fr = KF.Framer(4096)
    fr.feed(t)
    fr.flush(force=True)
    check('f9: the boat C0 -> STATUSTEXT (debug) "F9 C0 front 00 01 00 00 00 00 00 00 00 00 sats 38", whole MAVLink 2 '
          'for the relay',
          len(st) == 1 and st[0].get_type() == 'STATUSTEXT' and st[0].severity == 7
          and st[0].text == 'F9 C0 front 00 01 00 00 00 00 00 00 00 00 sats 38'
          and [(k, fl) for k, _, fl in fr.out] == [('F', KF.P_MAV2)], str((st, fr.out)))
    check('f9: F9 C4 without position -> only GPS_RAW_INT (no fix) and BATTERY_STATUS; never an ATTITUDE (C0 only)',
          got == ['GPS_RAW_INT', 'BATTERY_STATUS'] and F9.attitude(float('nan'), 0, 0) == (b'', 0), str(got))


def main():
    py_tests()
    meaning_tests()
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)
        if exe:
            c_tests(exe)
    failed = [n for n, ok in TA.results if not ok]
    print('\n%d checks, %d failed' % (len(TA.results), len(failed)))
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
