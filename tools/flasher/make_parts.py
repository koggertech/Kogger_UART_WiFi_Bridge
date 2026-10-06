#!/usr/bin/env python3
"""Copy the parts the flasher writes besides the firmware file - bootloader, partition table, first-boot OTA data -
from a UART build, with a manifest of their offsets, hashes and the flash settings (docs/FLASHER.md).

  python tools/flasher/make_parts.py                       # from firmware/build-uart
  python tools/flasher/make_parts.py --build <build dir>

The parts change only when the bootloader or the partition layout does; the firmware file (.ufww) is chosen in the
flasher. Run this after such a change, and the flasher writes the new parts.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
APP = 'headunit_wifi_bridge.bin'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--build', default=os.path.join(ROOT, 'firmware', 'build-uart'))
    a = ap.parse_args()
    args = open(os.path.join(a.build, 'flash_args')).read().split()
    flash = dict(mode=args[args.index('--flash_mode') + 1], freq=args[args.index('--flash_freq') + 1],
                 size=args[args.index('--flash_size') + 1])
    pairs = [(args[i], args[i + 1]) for i in range(len(args) - 1) if re.fullmatch(r'0x[0-9a-fA-F]+', args[i])]
    out = os.path.join(HERE, 'parts')
    os.makedirs(out, exist_ok=True)
    files, app_offset = {}, None
    for off, rel in pairs:
        if os.path.basename(rel) == APP:
            app_offset = off
            continue
        name = os.path.basename(rel)
        shutil.copyfile(os.path.join(a.build, rel), os.path.join(out, name))
        files[name] = dict(offset=off, sha256=hashlib.sha256(open(os.path.join(out, name), 'rb').read()).hexdigest())
    if app_offset is None:
        sys.exit('no %s in flash_args' % APP)
    ver = re.search(r'set\(PROJECT_VER "([^"]+)"\)', open(os.path.join(ROOT, 'firmware', 'CMakeLists.txt')).read())
    manifest = dict(firmware=ver.group(1) if ver else '?', app_offset=app_offset, flash=flash, files=files)
    json.dump(manifest, open(os.path.join(out, 'manifest.json'), 'w'), indent=1)
    print(json.dumps(manifest, indent=1))


if __name__ == '__main__':
    main()
