#!/usr/bin/env python3
"""Generate the hardware outputs from the KiCad project with kicad-cli (KiCad 10).

  python tools/make_hw_outputs.py                     # hardware/ -> hardware/outputs, docs/img
  python tools/make_hw_outputs.py --hardware DIR --img DIR --kicad-cli PATH

hardware/outputs/:
  WiFi_UART_Bridge_v3_schematic.pdf     the schematic
  WiFi_UART_Bridge_v3_bom.csv           BOM grouped by value, part number and footprint (test points excluded)
  WiFi_UART_Bridge_v3_pos.csv           pick-and-place, top side, mm, origin at the board's lower-left corner
  WiFi_UART_Bridge_v3_fab.zip           Gerbers (4 copper layers, mask, paste and silkscreen of both sides, board
                                        outline; Protel extensions), Excellon drill, drill map, Gerber job file,
                                        the pick-and-place and the BOM
ERC and DRC (with schematic parity) run too; their summary is printed, the reports are not kept.
docs/img/: schematic.svg and the 3D renders board_top.png, board_bottom.png, board_iso.png.
kicad-cli: --kicad-cli, else $KICAD_CLI, else kicad-cli on PATH, else the default Windows install path.
"""
import argparse
import atexit
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NAME = 'WiFi_UART_Bridge_v3'
LAYERS = 'F.Cu,In1.Cu,In2.Cu,B.Cu,F.Paste,B.Paste,F.SilkS,B.SilkS,F.Mask,B.Mask,Edge.Cuts'


def find_cli(arg):
    for c in (arg, os.environ.get('KICAD_CLI'), 'kicad-cli', r'C:\Program Files\KiCad\10.0\bin\kicad-cli.exe'):
        if c and (shutil.which(c) or os.path.isfile(c)):
            return shutil.which(c) or c
    sys.exit('kicad-cli not found: pass --kicad-cli or set KICAD_CLI')


def english_config():
    """Environment for kicad-cli with an English user interface: KiCad writes UI strings (page names, bookmarks)
    into the PDF. A copy of the user's kicad_common.json with language English, in a temporary config home."""
    import json
    home = tempfile.mkdtemp(prefix='kicad-cfg-')
    ver = os.path.join(home, '10.0')
    os.makedirs(ver)
    user = os.path.join(os.environ.get('APPDATA', os.path.expanduser('~/.config')), 'kicad', '10.0', 'kicad_common.json')
    cfg = json.load(open(user, encoding='utf-8')) if os.path.isfile(user) else {}
    cfg.setdefault('system', {})['language'] = 'English'
    json.dump(cfg, open(os.path.join(ver, 'kicad_common.json'), 'w', encoding='utf-8'))
    for table in ('sym-lib-table', 'fp-lib-table', 'design-block-lib-table'):  # the global libraries (power, ...)
        src = os.path.join(os.path.dirname(user), table)
        if os.path.isfile(src):
            shutil.copyfile(src, os.path.join(ver, table))
    env = dict(os.environ)
    env['KICAD_CONFIG_HOME'] = home
    return env, home


def check_no_cyrillic(pdf):
    try:
        import pymupdf
    except ImportError:
        print('note: pymupdf not installed, the PDF language check is skipped')
        return
    import re
    d = pymupdf.open(pdf)
    text = ''.join(page.get_text() for page in d) + str(d.get_toc()) + str(d.metadata)
    text += ''.join(d.xref_object(i) for i in range(1, d.xref_length()))
    if re.search('[\u0400-\u04ff]', text) or re.search(r'\\u04[0-9a-fA-F]{2}', text):
        sys.exit('%s contains Cyrillic text (KiCad UI language leaked into the PDF)' % pdf)


def summary(erc_path, drc_path):
    import collections
    import json
    erc = json.load(open(erc_path, encoding='utf-8'))
    drc = json.load(open(drc_path, encoding='utf-8'))
    e = collections.Counter(v['type'] for sh in erc.get('sheets', []) for v in sh.get('violations', []))
    d = collections.Counter('%s (%s)' % (v['type'], v['severity']) for v in drc.get('violations', []))
    print('ERC: %s' % (dict(e) or 'clean'))
    print('DRC: %s; unconnected %d; schematic parity %d' % (dict(d) or 'clean', len(drc.get('unconnected_items', [])),
          len(drc.get('schematic_parity', []))))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--hardware', default=os.path.join(ROOT, 'hardware'))
    ap.add_argument('--img', default=os.path.join(ROOT, 'docs', 'img'))
    ap.add_argument('--kicad-cli')
    a = ap.parse_args()
    cli = find_cli(a.kicad_cli)
    sch = os.path.join(a.hardware, NAME + '.kicad_sch')
    pcb = os.path.join(a.hardware, NAME + '.kicad_pcb')
    out = os.path.join(a.hardware, 'outputs')
    os.makedirs(out, exist_ok=True)
    os.makedirs(a.img, exist_ok=True)

    env, cfg_home = english_config()
    atexit.register(shutil.rmtree, cfg_home, ignore_errors=True)

    def run(*args):
        r = subprocess.run([cli] + list(args), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                           encoding='utf-8', errors='replace', env=env)
        if r.returncode:
            sys.exit('kicad-cli %s failed (%d):\n%s' % (' '.join(args[:3]), r.returncode, r.stdout[-3000:]))

    p = lambda f: os.path.join(out, NAME + f)
    run('sch', 'export', 'pdf', '--exclude-pdf-property-popups', '-o', p('_schematic.pdf'), sch)
    check_no_cyrillic(p('_schematic.pdf'))
    tmp = tempfile.mkdtemp(prefix='hw-')
    try:
        run('sch', 'export', 'svg', '-o', tmp, sch)
        shutil.copyfile(os.path.join(tmp, NAME + '.svg'), os.path.join(a.img, 'schematic.svg'))
        run('sch', 'export', 'bom', '-o', p('_bom.csv'),
            '--fields', 'Reference,Value,Footprint,MPN,Manufacturer,Description,${QUANTITY}',
            '--labels', 'References,Value,Footprint,MPN,Manufacturer,Description,Qty',
            '--group-by', 'Value,MPN,Footprint', '--ref-range-delimiter', '', '--exclude-dnp', sch)
        run('pcb', 'export', 'pos', '-o', p('_pos.csv'), '--side', 'front', '--format', 'csv', '--units', 'mm',
            '--use-drill-file-origin', '--exclude-dnp', pcb)
        fab = os.path.join(tmp, 'fab')
        os.makedirs(fab)
        run('pcb', 'export', 'gerbers', '-o', fab + os.sep, '--layers', LAYERS, '--use-drill-file-origin',
            '--subtract-soldermask', pcb)
        run('pcb', 'export', 'drill', '-o', fab + os.sep, '--format', 'excellon', '--drill-origin', 'plot',
            '--excellon-units', 'mm', '--generate-map', '--map-format', 'gerberx2', pcb)
        with zipfile.ZipFile(p('_fab.zip'), 'w', zipfile.ZIP_DEFLATED) as z:
            for f in sorted(os.listdir(fab)):
                z.write(os.path.join(fab, f), f)
            z.write(p('_pos.csv'), NAME + '_pos.csv')
            z.write(p('_bom.csv'), NAME + '_bom.csv')
        erc, drc = os.path.join(tmp, 'erc.json'), os.path.join(tmp, 'drc.json')
        run('sch', 'erc', '--format', 'json', '--severity-all', '-o', erc, sch)
        run('pcb', 'drc', '--format', 'json', '--severity-all', '--schematic-parity', '-o', drc, pcb)
        summary(erc, drc)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    common = ['--quality', 'high', '--use-board-stackup-colors', '--background', 'opaque', pcb]
    run('pcb', 'render', '-o', os.path.join(a.img, 'board_top.png'), '--side', 'top', '--width', '800',
        '--height', '1120', *common)
    run('pcb', 'render', '-o', os.path.join(a.img, 'board_bottom.png'), '--side', 'bottom', '--width', '800',
        '--height', '1120', *common)
    run('pcb', 'render', '-o', os.path.join(a.img, 'board_iso.png'), '--width', '1400', '--height', '1000',
        '--rotate', '-50,0,30', '--perspective', *common)
    for f in sorted(os.listdir(out)):
        print('%-44s %8d bytes' % ('hardware/outputs/' + f, os.path.getsize(os.path.join(out, f))))


if __name__ == '__main__':
    main()
