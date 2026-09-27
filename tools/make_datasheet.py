#!/usr/bin/env python3
"""Print the datasheet to PDF with a headless Chromium browser (Edge or Chrome).

  python tools/make_datasheet.py                  # docs/datasheet/datasheet.html -> docs/KoggerWiFi_datasheet.pdf
  python tools/make_datasheet.py --docs DIR       # another docs directory with the same layout
  python tools/make_datasheet.py --browser PATH

Before printing it checks that the datasheet names the firmware version from firmware/CMakeLists.txt and that
every image it references exists (charts: tools/make_charts.py; schematic and board renders:
tools/make_hw_outputs.py). Browser: --browser, else
$BROWSER_BIN, else msedge / chrome / chromium on PATH, else the usual Windows install paths.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CANDIDATES = ['msedge', 'microsoft-edge', 'google-chrome', 'chrome', 'chromium', 'chromium-browser',
              r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe',
              r'C:\Program Files\Microsoft\Edge\Application\msedge.exe',
              r'C:\Program Files\Google\Chrome\Application\chrome.exe']


def find_browser(arg):
    for c in [arg, os.environ.get('BROWSER_BIN')] + CANDIDATES:
        if not c:
            continue
        p = shutil.which(c) or (c if os.path.isfile(c) else None)
        if p:
            return p
    sys.exit('no Chromium-based browser found: pass --browser PATH or set BROWSER_BIN')


def check_frames(text):
    """Every <code class="frame"> must be a valid Kogger SBP KP1 frame, every <code class="slip"> a valid SLIP frame."""
    bad = []
    for hexs in re.findall(r'<code class="frame">([0-9A-F ]+)</code>', text):
        f = bytes.fromhex(hexs)
        c1 = c2 = 0
        for x in f[2:-2]:
            c1 = (c1 + x) & 0xFF
            c2 = (c2 + c1) & 0xFF
        if f[:2] != b'\xbb\x55' or len(f) != f[5] + 8 or (c1, c2) != (f[-2], f[-1]):
            bad.append(hexs)
    for hexs in re.findall(r'<code class="slip">([0-9A-F ]+)</code>', text):
        f = bytes.fromhex(hexs)
        body, raw, esc = bytearray(), f[1:-1], False
        for x in raw:
            if esc:
                body.append({0xDC: 0xC0, 0xDD: 0xDB}.get(x, 0x100) if x in (0xDC, 0xDD) else 0)
                esc = False
            elif x == 0xDB:
                esc = True
            else:
                body.append(x)
        crc = 0xFFFF
        for x in body[:-2]:
            crc ^= x << 8
            for _ in range(8):
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
        if f[0] != 0xC0 or f[-1] != 0xC0 or crc != (body[-2] << 8 | body[-1]):
            bad.append(hexs)
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--docs', default=os.path.join(ROOT, 'docs'))
    ap.add_argument('--browser')
    a = ap.parse_args()
    html = os.path.abspath(os.path.join(a.docs, 'datasheet', 'datasheet.html'))
    pdf = os.path.abspath(os.path.join(a.docs, 'KoggerWiFi_datasheet.pdf'))
    text = open(html, encoding='utf-8').read()

    ver = re.search(r'set\(PROJECT_VER "([^"]+)"\)', open(os.path.join(ROOT, 'firmware', 'CMakeLists.txt')).read()).group(1)
    if 'firmware %s' % ver not in text or 'Firmware <b>%s</b>' % ver not in text:
        sys.exit('datasheet does not name firmware %s (firmware/CMakeLists.txt): update it first' % ver)
    missing = [src for src in re.findall(r'<img src="([^"]+)"', text)
               if not os.path.isfile(os.path.join(os.path.dirname(html), src))]
    if missing:
        sys.exit('missing images: %s (run tools/make_charts.py for charts, tools/make_hw_outputs.py for the schematic '
                 'and board renders)' % ', '.join(missing))

    bad = check_frames(text)
    if bad:
        sys.exit('example frames with a wrong length or checksum: ' + ', '.join(bad))

    browser = find_browser(a.browser)
    profile = tempfile.mkdtemp(prefix='datasheet-')
    try:
        if os.path.exists(pdf):
            os.remove(pdf)
        cmd = [browser, '--headless', '--disable-gpu', '--no-first-run', '--no-default-browser-check',
               '--user-data-dir=' + profile, '--no-pdf-header-footer', '--print-to-pdf=' + pdf,
               'file:///' + html.replace(os.sep, '/')]
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=180)
    finally:
        shutil.rmtree(profile, ignore_errors=True)
    if not os.path.isfile(pdf) or open(pdf, 'rb').read(5) != b'%PDF-':
        sys.exit('printing failed (exit %d):\n%s' % (r.returncode, r.stdout[-2000:]))
    data = open(pdf, 'rb').read()
    pages = len(re.findall(rb'/Type\s*/Page[^s]', data))
    print('%s  %d bytes  %d pages' % (pdf, len(data), pages))


if __name__ == '__main__':
    main()
