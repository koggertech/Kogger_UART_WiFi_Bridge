#!/usr/bin/env python3
"""Pack the flasher into one KoggerWiFiFlasher.exe (PyInstaller) and check the packed copy with its --selftest.

  python tools/flasher/make_exe.py --lang en    -> dist/KoggerWiFiFlasher_EN.exe  (English window)
  python tools/flasher/make_exe.py --lang ru    -> dist/KoggerWiFiFlasher.exe     (Russian window, needs lang/ru.json)

Without --lang: Russian when lang/ru.json is present, else English. The self-test needs a firmware file: the newest
dist/KoggerWiFi_*.ufww, or --ufww <file> (a release file from GitHub).

The exe carries parts/ and esptool with its stub data, so it runs on a PC without Python: one file to copy. It is
written to dist/ (not in git); PyInstaller's work files go to the temp folder. The self-test opens no window and no
real port: the file check, the parts, esptool's ESP32-C3 stub data, Tk, and the 'no port' / 'not found' paths with
esptool really run (docs/FLASHER.md). Needs PyInstaller: python -m pip install pyinstaller.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DIST = os.path.join(ROOT, 'dist')
WORK = os.path.join(tempfile.gettempdir(), 'kogger_flasher_build')
NAME = 'KoggerWiFiFlasher'


def newest_ufww():
    best = None
    for f in os.listdir(DIST) if os.path.isdir(DIST) else []:
        m = re.fullmatch(r'KoggerWiFi_(\d+)\.(\d+)\.(\d+)\.ufww', f)
        if m and (best is None or tuple(map(int, m.groups())) > best[0]):
            best = (tuple(map(int, m.groups())), os.path.join(DIST, f))
    return best and best[1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--lang', choices=('ru', 'en'),
                    default='ru' if os.path.isfile(os.path.join(HERE, 'lang', 'ru.json')) else 'en')
    ap.add_argument('--ufww', help='firmware file for the self-test (default: the newest in dist/)')
    a = ap.parse_args()
    lang = a.lang
    name = NAME if lang == 'ru' else NAME + '_' + lang.upper()
    work = os.path.join(WORK, lang)
    os.makedirs(work, exist_ok=True)
    with open(os.path.join(work, 'lang.txt'), 'w', encoding='ascii') as f:
        f.write(lang)
    cmd = [sys.executable, '-m', 'PyInstaller', '--noconfirm', '--clean', '--onefile', '--windowed', '--name', name,
           '--add-data', os.path.join(HERE, 'parts') + os.pathsep + 'parts',
           '--add-data', os.path.join(HERE, 'lang') + os.pathsep + 'lang',
           '--add-data', os.path.join(work, 'lang.txt') + os.pathsep + '.',
           '--collect-data', 'esptool', '--collect-submodules', 'esptool',
           '--workpath', os.path.join(work, 'work'), '--specpath', work, '--distpath', DIST,
           os.path.join(HERE, 'kogger_wifi_flasher.pyw')]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(r.stdout[-3000:], r.stderr[-3000:])
        sys.exit('PyInstaller failed (%d)' % r.returncode)
    exe = os.path.join(DIST, name + '.exe')
    ufww = a.ufww or newest_ufww()
    if not ufww:
        sys.exit('no KoggerWiFi_*.ufww in dist/ to check the exe against: give one with --ufww')
    report = os.path.join(work, 'selftest.json')
    if os.path.exists(report):
        os.remove(report)
    rc = subprocess.run([exe, '--selftest', ufww, report], timeout=240).returncode
    res = json.load(open(report, encoding='utf-8')) if os.path.exists(report) else {}
    print(json.dumps(res, indent=1, ensure_ascii=False))
    ok = rc == 0 and res.get('ok') is True and res.get('lang') == lang  # the packed lang.txt took effect
    print('%s  %d bytes  lang %s  self-test %s (against %s)' % (exe, os.path.getsize(exe), res.get('lang'),
                                                                'PASS' if ok else 'FAIL', os.path.basename(ufww)))
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
