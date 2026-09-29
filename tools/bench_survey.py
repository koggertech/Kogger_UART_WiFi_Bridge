#!/usr/bin/env python3
"""Bench checks for the channel survey (ID_WIFI_SURVEY 0x59, firmware 0.16, docs/SBP_WIFI.md §4a).

  python tools/bench_survey.py --port COM3                 # sweep every channel, 120 ms each
  python tools/bench_survey.py --port COM3 --dwell 200 --channels 1,6,11
  python tools/bench_survey.py --port /dev/ttyUSB0 --baud 2000000

The module is found by discovery, as KoggerApp finds it, so its address need not be known. The sweep takes
the radio away for about (channels x dwell) ms: a Wi-Fi link pauses and should come back by itself. Every
setting is left as it was - the sweep changes none. Exit code 0 = all checks PASS.
"""
import argparse
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'host'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import sbpframe as SB  # noqa: E402
from bench_sbp import Dev, check, results  # noqa: E402
from module_state import find_module  # noqa: E402

V = 0  # the only version of this id so far


def pages_of(d, timeout):
    """Every page of one sweep: stop as soon as the first page's `total` of them have arrived."""
    pages, end = [], time.time() + timeout
    while time.time() < end:
        f = d.wait(lambda f: f.id == SB.ID_WIFI_SURVEY and f.ver == V and f.type == SB.CONTENT
                   and not f.resp, end - time.time())
        if f is None:
            break
        pages.append(SB.parse_survey(f.payload))
        if len(pages) >= pages[0].get('total', 0):
            break
    return pages


def show(pages):
    print('  ch  dwell  busy%%  frames  senders  rssi  noise  flags')
    for p in sorted(pages, key=lambda x: x['channel']):
        fl = ' '.join(k for k, v in p['flags'].items() if v)
        print('  %2d  %5d  %5.1f  %6d  %7d  %4d  %5d  %s' % (p['channel'], p['dwell_ms'],
              p['busy_permille'] / 10.0, p['frames'], p['senders'], p['rssi'], p['noise'], fl))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', required=True)
    ap.add_argument('--baud', type=int, default=921600)
    ap.add_argument('--dwell', type=int, default=0, help='ms per channel, 0 = the module default')
    ap.add_argument('--channels', default='', help='e.g. 1,6,11; empty = every channel the module allows')
    a = ap.parse_args()

    mask = 0
    for part in [x for x in a.channels.split(',') if x.strip()]:
        mask |= 1 << (int(part) - 1)
    d = Dev(a.port, a.baud)
    d.pump(0.3)
    route, v2 = find_module(d)
    if not check('the module answers discovery', route is not None):
        sys.exit(1)
    d.route = route
    fw = (v2[8], v2[7])
    check('firmware %d.%d has the survey (0.16 or later)' % fw, fw >= (0, 16), '%d.%d' % fw)

    code, _ = d.setting(SB.ID_WIFI_SURVEY, V, SB.survey_payload(a.dwell, mask))
    if not check('SETTING v0 {KEY, dwell, channels} -> OK', code == 1, str(code)):
        sys.exit(1)
    busy_code, _ = d.setting(SB.ID_WIFI_SURVEY, V, SB.survey_payload(a.dwell, mask), timeout=1.0)
    check('a second sweep while one runs -> ERR_RUNTIME', busy_code == 8, str(busy_code))

    t0 = time.time()
    pages = pages_of(d, 20)
    took = time.time() - t0
    if not check('the results arrive (%d channel pages in %.1f s)' % (len(pages), took), pages):
        sys.exit(1)
    show(pages)

    total = pages[0]['total']
    want = bin(mask).count('1') if mask else SB.SURVEY_CH_SCAN_MAX
    check('one page per measured channel, %d of them' % want,
          total == want and len(pages) == want and sorted(p['index'] for p in pages) == list(range(want)),
          '%d pages, total field %d' % (len(pages), total))
    dwell = a.dwell or SB.SURVEY_DWELL_DEF
    check('every page reports the dwell that was asked (%d ms), bar the module own channel' % dwell,
          all(p['dwell_ms'] == dwell for p in pages if not p['flags']['home']),
          str([(p['channel'], p['dwell_ms']) for p in pages]))
    check('busy is within 0..1000 per mille', all(0 <= p['busy_permille'] <= 1000 for p in pages))
    check('a channel that heard nothing reports RSSI -128 and no transmitters',
          all(p['rssi'] == -128 and p['senders'] == 0 for p in pages if p['frames'] == 0))
    check('channels 1..11 are flagged as usable by an access point',
          all(p['flags']['ap_ok'] == (p['channel'] <= SB.SURVEY_CH_SCAN_MAX) for p in pages))
    homes = [p['channel'] for p in pages if p['flags']['home']]
    check('at most one channel is flagged as the module own', len(homes) <= 1, str(homes))
    heard = [p for p in pages if p['frames']]
    check('at least one channel heard something (there is Wi-Fi around)', heard)
    check('a frame implies air time on that channel', all(p['busy_permille'] > 0 for p in heard),
          str([(p['channel'], p['frames'], p['busy_permille']) for p in heard if not p['busy_permille']]))
    home = [p for p in pages if p['flags']['home']]
    check('the module own channel counts the time the radio came back to it between channels (0.16)',
          all(p['dwell_ms'] > dwell for p in home), str([(p['channel'], p['dwell_ms']) for p in home]))

    again = d.get(SB.ID_WIFI_SURVEY, V, timeout=3)
    more = ([SB.parse_survey(again.payload)] + pages_of(d, 3)) if again else []
    check('GETTING v0 returns the same results again', len(more) == len(pages)
          and [p['channel'] for p in sorted(more, key=lambda x: x['channel'])] ==
              [p['channel'] for p in sorted(pages, key=lambda x: x['channel'])], '%d pages' % len(more))

    code, _ = d.setting(SB.ID_WIFI_SURVEY, V, b'\x00\x00\x00\x00\x00\x00\x00\x00')
    check('a wrong key is refused (ERR_KEY)', code == 7, str(code))
    code, _ = d.setting(SB.ID_WIFI_SURVEY, V, SB.KEY)
    check('a payload without dwell and channels is refused (ERR_PAYLOAD)', code == 3, str(code))
    code, _ = d.setting(SB.ID_WIFI_SURVEY, 1, SB.survey_payload())
    check('an unknown version is refused (ERR_VERSION)', code == 5, str(code))

    failed = [n for n, ok in results if not ok]
    print('%d checks, %d failed' % (len(results), len(failed)))
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
