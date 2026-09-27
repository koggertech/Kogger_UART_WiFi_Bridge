#!/usr/bin/env python3
"""Package a built firmware as an update file for KoggerApp / tools/sbp_update.py.

  python tools/make_update.py                                   # from firmware/build-uart
  python tools/make_update.py --build firmware/build-v099 --out dist/test

The file is the ESP-IDF application image unchanged (it carries its own chip id, project name,
version and SHA-256); it is named KoggerWiFi_<version>.ufww. Before writing it the script checks what
the module will check, so a wrong file is caught here first. See docs/UPDATE.md.
"""
import argparse
import hashlib
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJECT = 'headunit_wifi_bridge'
CHIP_ESP32C3 = 5
DESC_MAGIC = 0xABCD5432


def inspect(data):
    """Return (version, project) or raise ValueError, applying the module's own checks."""
    if len(data) < 256:
        raise ValueError('too short for an image')
    if data[0] != 0xE9:
        raise ValueError('not an ESP image (magic %02x)' % data[0])
    chip = struct.unpack_from('<H', data, 12)[0]
    if chip != CHIP_ESP32C3:
        raise ValueError('image for chip id %d, not ESP32-C3' % chip)
    d = 24 + 8
    magic = struct.unpack_from('<I', data, d)[0]
    if magic != DESC_MAGIC:
        raise ValueError('no application descriptor')
    version = data[d + 16:d + 48].split(b'\0')[0].decode()
    project = data[d + 48:d + 80].split(b'\0')[0].decode()
    if project != PROJECT:
        raise ValueError('project %r, expected %r' % (project, PROJECT))
    if data[23] != 1:
        raise ValueError('image has no appended SHA-256')
    if hashlib.sha256(data[:-32]).digest() != data[-32:]:
        raise ValueError('appended SHA-256 does not match the image')
    return version, project


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--build', default=os.path.join(ROOT, 'firmware', 'build-uart'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'dist'))
    a = ap.parse_args()
    src = os.path.join(a.build, PROJECT + '.bin')
    data = open(src, 'rb').read()
    try:
        version, _ = inspect(data)
    except ValueError as e:
        sys.exit('refused: %s (%s)' % (e, src))
    os.makedirs(a.out, exist_ok=True)
    dst = os.path.join(a.out, 'KoggerWiFi_%s.ufww' % version)
    with open(dst, 'wb') as f:
        f.write(data)
    print('%s  %d bytes  version %s  sha256 %s' % (dst, len(data), version, hashlib.sha256(data).hexdigest()))


if __name__ == '__main__':
    main()
