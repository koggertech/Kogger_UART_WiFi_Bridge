#!/usr/bin/env python3
"""Generate the documentation charts (docs/img/*.svg).

  python tools/make_charts.py               # -> docs/img
  python tools/make_charts.py --out DIR

Where the numbers come from:
  - buffer sizes, rescan budget, update timing: parsed from firmware/main (*.c, kframe.h, Kconfig.projbuild);
  - flash map: firmware/partitions.csv;
  - rescan amplification: measured here by running host/kframe.py (the Python mirror that tests/test_all.py
    proves byte-equal to the firmware's C) on the PC test streams and on a stream of false syncs;
  - TX power steps: the table of esp_wifi_set_max_tx_power() in ESP-IDF 5.5.5 esp_wifi.h (TX_POWER_TABLE);
  - bench and field measurements: constants in the MEASURED block, the same figures as docs/DESIGN.md.
The output is deterministic for a given matplotlib version: fixed hash salt, no dates.
Needs matplotlib.
"""
import argparse
import csv
import io
import os
import random
import re
import sys

import matplotlib

matplotlib.use('Agg')
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FW = os.path.join(ROOT, 'firmware')
sys.path.insert(0, os.path.join(ROOT, 'host'))
sys.path.insert(0, os.path.join(ROOT, 'tests'))

# esp_wifi.h (ESP-IDF 5.5.5), esp_wifi_set_max_tx_power(): {set value range, actual value}, unit 0.25 dBm
TX_POWER_TABLE = [((8, 19), 8), ((20, 27), 20), ((28, 33), 28), ((34, 43), 34), ((44, 51), 44), ((52, 55), 52),
                  ((56, 59), 56), ((60, 65), 60), ((66, 71), 66), ((72, 79), 72), ((80, 84), 80)]

# MEASURED: bench CP2105 USB-UART at 921600 baud unless stated (docs/DESIGN.md, docs/UPDATE.md)
RTT_MS = {56: (4.9, 6.1, 17.4), 1400: (40.4, 41.4, 46.3)}   # ICMP payload -> min/avg/max through NAPT
BENCH_BAUDS = (9600, 2000000)                                  # 28/28 switches via ID_UART
HEAD_UNIT_BAUDS = (2000000, 3000000, 4000000)                  # set over SBP, field
FIELD_STREAM_KBPS = 92                                         # boat stream that saturated 921600
OTA_TRANSFER_S = 80                                            # 885 120 bytes, 0.3.0 -> 0.3.1
IMAGE_BYTES = 932512                                           # 0.11.0, UART build

C_BLUE, C_ORANGE, C_GREEN, C_GREY, C_RED, C_LIGHT = '#1f5f8b', '#d9822b', '#3a8d5d', '#7a7a7a', '#b23b3b', '#e8eef3'
STD_BAUDS = [9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 1000000, 1500000, 2000000, 3000000, 4000000]


def read(rel):
    with open(os.path.join(ROOT, rel), encoding='utf-8') as f:
        return f.read()


def define(rel, name):
    """Integer value of '#define NAME expr' (expr: numbers, LL suffixes, * + - / and parentheses)."""
    m = re.search(r'^#define\s+' + name + r'\s+(.+?)(?:/\*.*)?$', read(rel), re.M)
    if not m:
        raise SystemExit('%s: #define %s not found' % (rel, name))
    expr = re.sub(r'(?<=\d)LL\b', '', m.group(1)).strip()
    if not re.fullmatch(r'[0-9\s()*+/-]+', expr):
        raise SystemExit('%s: %s = %r is not a plain number' % (rel, name, expr))
    return int(eval(expr))  # digits and operators only, checked above


def kconfig_default(name):
    m = re.search(r'config ' + name + r'\b.*?\n\s*default\s+(\d+)', read('firmware/main/Kconfig.projbuild'), re.S)
    if not m:
        raise SystemExit('Kconfig: default of %s not found' % name)
    return int(m.group(1))


def style():
    plt.rcParams.update({
        'svg.hashsalt': 'kogger-wifi-bridge', 'svg.fonttype': 'path', 'font.family': 'DejaVu Sans',
        'font.size': 9, 'axes.titlesize': 10, 'axes.titleweight': 'bold', 'axes.spines.top': False,
        'axes.spines.right': False, 'axes.grid': True, 'grid.color': '#dddddd', 'grid.linewidth': 0.6,
        'legend.frameon': False, 'figure.dpi': 100,
    })


FORMAT = 'svg'


def save(fig, out, name):
    path = os.path.join(out, name if FORMAT == 'svg' else name[:-3] + FORMAT)
    meta = {'Date': None, 'Creator': None} if FORMAT == 'svg' else None
    buf = io.BytesIO()
    fig.savefig(buf, format=FORMAT, bbox_inches='tight', metadata=meta, dpi=150)
    data = buf.getvalue()
    if FORMAT == 'svg':
        data = data.replace(b'\r\n', b'\n')  # matplotlib writes the platform's line ends; keep files identical
    with open(path, 'wb') as f:
        f.write(data)
    plt.close(fig)
    print('wrote', path)


def fmt_baud(b):
    return '%g M' % (b / 1e6) if b >= 1000000 else '%g k' % (b / 1e3)


def chart_uart_throughput(out):
    fig, ax = plt.subplots(figsize=(7.4, 3.5))
    xs = [b for b in range(9600, 4000001, 9600)]
    ax.plot(xs, [b / 10 / 1000 for b in xs], color=C_BLUE, lw=1.8, label='8N1 capacity: baud / 10 bytes/s, each way')
    ax.axvspan(*BENCH_BAUDS, color='#dde7f0', zorder=0, label='rates verified on a bench adapter')
    ax.plot(HEAD_UNIT_BAUDS, [b / 10 / 1000 for b in HEAD_UNIT_BAUDS], 'o', color=C_GREEN, ms=6,
            label='rates verified on a head unit UART')
    ax.plot([921600], [FIELD_STREAM_KBPS], 's', color=C_RED, ms=6,
            label='field: a %d KB/s stream saturated 921600' % FIELD_STREAM_KBPS)
    for b in (115200, 921600, 2000000, 4000000):
        ax.plot([b], [b / 10 / 1000], 'o', color=C_BLUE, ms=3)
        ax.annotate('%.1f KB/s' % (b / 10 / 1000) if b < 1000000 else '%.0f KB/s' % (b / 10 / 1000),
                    (b, b / 10 / 1000), textcoords='offset points', xytext=(6, -12), ha='left', fontsize=8)
    ticks = [9600, 38400, 115200, 460800, 921600, 2000000, 4000000]
    ax.set_xscale('log')
    ax.set_yscale('log')
    ax.set_xlim(8000, 6000000)
    ax.set_ylim(0.5, 1500)
    ax.set_xticks(ticks)
    ax.set_xticklabels([fmt_baud(b) for b in ticks], fontsize=8)
    ax.minorticks_off()
    ax.set_xlabel('baud rate')
    ax.set_ylabel('payload, KB/s')
    ax.set_title('Serial line throughput (the bottleneck of the relay)')
    ax.legend(loc='upper left', fontsize=8)
    save(fig, out, 'uart_throughput.svg')


def chart_rx_ring(out):
    ring0 = define('firmware/main/link.c', 'UART_RX_RING')
    ring1 = define('firmware/main/uline.c', 'RX_RING')
    fig, ax = plt.subplots(figsize=(7.4, 3.4))
    xs = [b for b in range(9600, 4000001, 9600)]
    for ring, col, lab in ((ring0, C_BLUE, 'line 0 (X1): %d KiB driver ring' % (ring0 // 1024)),
                           (ring1, C_ORANGE, 'line 1 (X2): %d KiB driver ring' % (ring1 // 1024))):
        ax.plot(xs, [ring / (b / 10) * 1000 for b in xs], color=col, lw=1.8, label=lab)
    for ring, b, col in ((ring0, 921600, C_BLUE), (ring0, 4000000, C_BLUE), (ring1, 115200, C_ORANGE)):
        t = ring / (b / 10) * 1000
        ax.plot([b], [t], 'o', color=col, ms=4)
        ax.annotate('%.0f ms at %s' % (t, fmt_baud(b)), (b, t), textcoords='offset points', xytext=(6, 4), fontsize=8)
    ticks = [9600, 38400, 115200, 460800, 921600, 2000000, 4000000]
    ax.set_xscale('log')
    ax.set_yscale('log')
    ax.set_xlim(8000, 6000000)
    ax.set_xticks(ticks)
    ax.set_xticklabels([fmt_baud(b) for b in ticks], fontsize=8)
    ax.minorticks_off()
    ax.set_xlabel('baud rate')
    ax.set_ylabel('time to fill, ms')
    ax.set_title('How long the receive ring holds a full-rate stream if the task stalls')
    ax.legend(loc='upper right', fontsize=8)
    save(fig, out, 'rx_ring_hold.svg')


def chart_tx_power(out):
    fig, ax = plt.subplots(figsize=(7.4, 3.3))
    xs, ys = [], []
    for (lo, hi), act in TX_POWER_TABLE:
        for v in range(lo, hi + 1):
            xs.append(v / 4)
            ys.append(act / 4)
    ax.step(xs, ys, where='post', color=C_BLUE, lw=1.8, label='applied by the driver')
    ax.plot([2, 21], [2, 21], ls='--', color=C_GREY, lw=0.8, label='requested = applied')
    steps = sorted({a for _, a in TX_POWER_TABLE})
    ax.set_yticks([a / 4 for a in steps])
    ax.set_yticklabels(['%g' % (a / 4) for a in steps], fontsize=8)
    ax.axvline(20, color=C_RED, lw=0.8)
    ax.text(19.8, 3.2, 'requests of 81-84 (20.25-21 dBm)\nare stored as 80 (20 dBm)', fontsize=8, color=C_RED,
            ha='right')
    ax.set_xlim(1.5, 21.5)
    ax.set_xlabel('requested power limit, dBm (ID_WIFI v7 value / 4)')
    ax.set_ylabel('applied, dBm')
    ax.set_title('Transmit power steps (esp_wifi_set_max_tx_power, ESP-IDF 5.5)')
    ax.legend(loc='upper left', fontsize=8)
    save(fig, out, 'tx_power_steps.svg')


def chart_ota_timeline(out):
    window = define('firmware/main/ota.c', 'WINDOW_US') / 1e6
    stall = define('firmware/main/ota.c', 'IDLE_US') / 1e6
    confirm_min = define('firmware/main/ota.c', 'CONFIRM_MIN_US') / 1e6
    deadline = kconfig_default('WB_OTA_CONFIRM_S')
    t_check = 1 + OTA_TRANSFER_S
    t_boot = t_check + 3
    fig, ax = plt.subplots(figsize=(7.4, 3.0))
    rows = [  # row, start, width, colour, label, label inside
        (4, 0, window, C_GREY, 'update window %g s: no chunk -> plain reboot' % window, False),
        (3, 1, OTA_TRANSFER_S, C_BLUE, 'chunks, %d s on the bench (%g s without a chunk: abandoned)' % (OTA_TRANSFER_S, stall), False),
        (2, t_check, 3, C_ORANGE, 'SHA-256 of the whole image, slot switch, reboot', False),
        (1, t_boot, confirm_min, C_LIGHT, 'must run %g s' % confirm_min, True),
        (1, t_boot + confirm_min, deadline - confirm_min, '#cfe8d6', 'confirms at the first host frame', True),
    ]
    for y, x0, w, col, lab, inside in rows:
        ax.barh(y, w, left=x0, height=0.55, color=col, edgecolor='#555555', lw=0.6)
        if inside:
            ax.text(x0 + w / 2, y, lab, va='center', ha='center', fontsize=8)
        else:
            ax.text(x0 + w + 3, y, lab, va='center', ha='left', fontsize=8)
    ax.axvline(t_boot + deadline, color=C_RED, lw=1.2)
    ax.text(t_boot + deadline + 3, 1.0, 'deadline %d s after boot:\nnot confirmed -> reboot,\nbootloader rolls back' %
            deadline, fontsize=8, color=C_RED, va='center')
    ax.set_yticks([1, 2, 3, 4])
    ax.set_yticklabels(['new image', 'check', 'transfer', 'ID_BOOT v0'])
    ax.set_xlim(0, t_boot + deadline + 80)
    ax.set_ylim(0.4, 4.6)
    ax.set_xlabel('seconds from ID_BOOT v0')
    ax.set_title('Firmware update over SBP: timeline and rollback')
    ax.grid(axis='y', visible=False)
    save(fig, out, 'ota_timeline.svg')


def chart_flash_map(out, image_bytes):
    parts = []
    with open(os.path.join(FW, 'partitions.csv'), encoding='utf-8') as f:
        for row in csv.reader(l for l in f if l.strip() and not l.startswith('#')):
            name, _, _, off, size = [c.strip() for c in row[:5]]
            parts.append((name, int(off, 16), int(size, 16)))
    total = 4 * 1024 * 1024
    end = max(o + s for _, o, s in parts)
    fig, ax = plt.subplots(figsize=(7.4, 2.0))
    lay = [('bootloader + table', 0, parts[0][1], C_GREY)] + [
        (n, o, s, C_LIGHT if n.startswith('ota_') else C_ORANGE) for n, o, s in parts]
    if end < total:
        lay.append(('free', end, total - end, '#f2f2f2'))
    for name, off, size, col in lay:
        ax.barh(0, size, left=off, height=0.7, color=col, edgecolor='#555555', lw=0.6)
        if size > 0x100000:
            ax.text(off + size / 2, 0.16, '%s  %.3f MiB' % (name, size / 1048576), ha='center', va='center', fontsize=8)
    ota0 = [p for p in parts if p[0] == 'ota_0'][0]
    ax.barh(-0.14, image_bytes, left=ota0[1], height=0.26, color=C_BLUE)
    ax.text(ota0[1] + image_bytes + 20000, -0.14, 'image %.2f MB = %d %% of a slot' %
            (image_bytes / 1e6, round(100 * image_bytes / ota0[2])), va='center', fontsize=8, color=C_BLUE)
    small = [p for p in lay if p[2] <= 0x100000]
    ax.text(0, 0.52, ',  '.join('%s 0x%X (%d KiB)' % (n, o, s // 1024) for n, o, s, _ in small), fontsize=7,
            color='#333333')
    ax.set_xlim(0, total)
    ax.set_ylim(-0.45, 0.72)
    ax.set_yticks([])
    ax.set_xticks([i * 0x80000 for i in range(9)])
    ax.set_xticklabels(['%g MB' % (i * 0.5) for i in range(9)], fontsize=8)
    ax.grid(False)
    ax.spines['left'].set_visible(False)
    ax.set_title('Flash layout (4 MB, partitions.csv)')
    save(fig, out, 'flash_map.svg')


class _Meter:
    """Counts bytes rescanned by host/kframe.py's Framer, with or without the rescan budget."""

    def __init__(self, KF, cap, budget):
        class M(KF.Framer):
            def _reject(s):
                n = len(s.buf) - 1
                if n <= s.budget:
                    s.rescanned += n
                super()._reject()
        self.f = M(cap)
        self.f.rescanned = 0
        if not budget:
            self.f.budget = float('inf')

    def run(self, stream):
        f = self.f
        for i in range(0, len(stream), 41):  # the feeding schedule of tests/test_all.py py_relay()
            f.feed(stream[i:i + 41])
            if (i // 41 + 1) % 24 == 0:
                f.flush()
        f.flush(force=True)
        return f.rescanned / len(stream)


def chart_rescan(out):
    import kframe as KF
    import test_all as T
    cap = T.RELAY_CAP
    ratio = define('firmware/main/kframe.h', 'KF_BUDGET_RATIO')
    save_x = define('firmware/main/kframe.h', 'KF_BUDGET_SAVE')
    if (ratio, save_x) != (KF.BUDGET_RATIO, KF.BUDGET_SAVE):
        raise SystemExit('host/kframe.py budget differs from firmware/main/kframe.h')
    sync = b'\xb5\x62\x00\x00\xf0\x0f'  # UBX header announcing 4080 bytes: every 6 bytes a new ~4 KB candidate
    lens = [2048, 4096, 8192, 16384, 32768, 65536]
    with_b, without_b = [], []
    for n in lens:
        s = (sync * (n // 6 + 1))[:n]
        with_b.append(_Meter(KF, cap, True).run(s))
        without_b.append(_Meter(KF, cap, False).run(s))
    rng = random.Random(5)
    bars = [('random bytes', bytes(rng.randrange(256) for _ in range(60000))),
            ('PC test stream, clean', T.relay_stream(random.Random(21), False)[0]),
            ('PC test stream, hostile', T.relay_stream(random.Random(22), True)[0])]
    bar_vals = [(lab, _Meter(KF, cap, True).run(s), _Meter(KF, cap, False).run(s)) for lab, s in bars]

    fig, (a1, a2) = plt.subplots(1, 2, figsize=(7.6, 3.4), gridspec_kw={'width_ratios': [1.5, 1]})
    a1.plot([n / 1024 for n in lens], without_b, 'o-', color=C_RED, lw=1.6, ms=4, label='without budget')
    a1.plot([n / 1024 for n in lens], with_b, 'o-', color=C_BLUE, lw=1.6, ms=4, label='with budget (0.11)')
    a1.axhline((cap - 9) / 6, color=C_RED, ls=':', lw=0.8)
    a1.text(2, (cap - 9) / 6 * 1.12, 'limit ~%d' % round((cap - 9) / 6), fontsize=8, color=C_RED)
    bound = [ratio + save_x * cap / n for n in lens]
    a1.plot([n / 1024 for n in lens], bound, ls='--', color=C_BLUE, lw=0.8, label='bound %d + %d x %d / length' % (ratio, save_x, cap))
    for n, v in ((lens[-1], without_b[-1]), (lens[-1], with_b[-1])):
        a1.annotate('%.0f x' % v if v > 20 else '%.1f x' % v, (n / 1024, v), textcoords='offset points', xytext=(-4, 6),
                    ha='right', fontsize=8)
    a1.set_xscale('log', base=2)
    a1.set_yscale('log')
    a1.set_xticks([n / 1024 for n in lens])
    a1.set_xticklabels(['%d' % (n // 1024) for n in lens], fontsize=8)
    a1.minorticks_off()
    a1.set_xlabel('stream of false UBX syncs, KB')
    a1.set_ylabel('rescanned bytes per input byte')
    a1.set_title('Hostile input')
    a1.legend(loc='center right', fontsize=7.5)
    ys = range(len(bar_vals))
    a2.barh([y + 0.2 for y in ys], [v[1] for v in bar_vals], height=0.38, color=C_BLUE, label='with budget')
    a2.barh([y - 0.2 for y in ys], [v[2] for v in bar_vals], height=0.38, color=C_RED, alpha=0.55, label='without')
    for y, (_, a, _b) in zip(ys, bar_vals):
        a2.text(a + 0.15, y + 0.2, '%.2f' % a, va='center', fontsize=8)
    a2.set_yticks(list(ys))
    a2.set_yticklabels([v[0] for v in bar_vals], fontsize=8)
    a2.set_xlabel('rescanned bytes per input byte')
    a2.set_title('Normal input: no change')
    a2.set_xlim(0, max(v[2] for v in bar_vals) * 1.3)
    a2.legend(loc='lower right', fontsize=7.5)
    fig.suptitle('Relay framer: rescan budget (measured on host/kframe.py)', fontsize=10, fontweight='bold')
    fig.tight_layout()
    save(fig, out, 'rescan_budget.svg')
    return lens, with_b, without_b, bar_vals


def chart_bridge_rtt(out):
    baud = 921600
    fig, ax = plt.subplots(figsize=(7.4, 2.8))
    labels = []
    for i, (payload, (mn, avg, mx)) in enumerate(sorted(RTT_MS.items())):
        ip = payload + 8 + 20
        frame = ip + 5  # SLIP: type, CRC-16, two delimiters (escapes not counted)
        wire = 2 * frame * 10 / baud * 1000
        ax.barh(i, wire, color=C_ORANGE, height=0.5, label='UART time at 921600 (model)' if i == 0 else None)
        ax.barh(i, avg - wire, left=wire, color=C_BLUE, height=0.5, label='rest: Wi-Fi, NAPT, stacks' if i == 0 else None)
        ax.plot([mn, mx], [i, i], color='#333333', lw=1)
        ax.plot([mn, mx], [i, i], '|', color='#333333', ms=10)
        ax.text(mx + 0.8, i, 'min %.1f / avg %.1f / max %.1f ms' % (mn, avg, mx), va='center', fontsize=8)
        labels.append('%d B ping\n(IP %d B)' % (payload, ip))
    ax.set_yticks(range(len(labels)))
    ax.set_yticklabels(labels, fontsize=8)
    ax.set_xlim(0, 75)
    ax.set_xlabel('round-trip time, ms (30 pings each)')
    ax.set_title('IP bridge: ping through NAPT to the access point (bench, 921600 baud)')
    ax.legend(loc='lower right', fontsize=8)
    ax.grid(axis='y', visible=False)
    save(fig, out, 'bridge_rtt.svg')


def box(ax, x, y, w, h, text, fc='#ffffff', ec=C_BLUE, fs=8, bold=False):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle='round,pad=0.02,rounding_size=0.08', fc=fc, ec=ec, lw=1.1))
    ax.text(x + w / 2, y + h / 2, text, ha='center', va='center', fontsize=fs, fontweight='bold' if bold else 'normal')


def arrow(ax, p, q, both=True, col='#444444', text=None, off=(0, 0.12)):
    ax.add_patch(FancyArrowPatch(p, q, arrowstyle='<|-|>' if both else '-|>', mutation_scale=9, color=col, lw=1.1,
                                 shrinkA=0, shrinkB=0))
    if text:
        ax.text((p[0] + q[0]) / 2 + off[0], (p[1] + q[1]) / 2 + off[1], text, ha='center', va='bottom', fontsize=7.2,
                color='#333333')


def chart_architecture(out):
    fig, ax = plt.subplots(figsize=(8.4, 4.2))
    ax.set_xlim(0, 10.5)
    ax.set_ylim(-0.35, 5.2)
    ax.axis('off')
    box(ax, 0.05, 3.55, 1.8, 1.0, 'Host\nKoggerApp or\nLinux daemon', fc=C_LIGHT, ec=C_GREY)
    box(ax, 0.05, 1.2, 1.8, 1.0, 'Device\nsonar, GNSS,\nautopilot', fc=C_LIGHT, ec=C_GREY)
    ax.add_patch(FancyBboxPatch((2.45, 0.3), 5.55, 4.6, boxstyle='round,pad=0.02,rounding_size=0.12', fc='#fbfcfd',
                                ec=C_BLUE, lw=1.6))
    ax.text(5.22, 4.68, 'ESP32-C3 module (this firmware)', ha='center', fontsize=9, fontweight='bold', color=C_BLUE)
    box(ax, 2.65, 3.6, 1.25, 0.9, 'line 0\nUART0 or USB')
    box(ax, 2.65, 1.25, 1.25, 0.9, 'line 1\nUART1')
    ax.add_patch(FancyBboxPatch((4.15, 0.55), 0.6, 3.95, boxstyle='round,pad=0.02,rounding_size=0.08', fc='#ffffff',
                                ec=C_BLUE, lw=1.1))
    ax.text(4.45, 2.52, 'frame splitter\nKP1, KP2, UBX, MAVLink, SLIP', rotation=90, ha='center', va='center',
            fontsize=7, linespacing=1.3)
    box(ax, 5.0, 3.45, 1.65, 1.05, 'manager\nWi-Fi, settings,\nSBP, update', fs=7.5)
    box(ax, 5.0, 2.0, 1.65, 1.05, 'relay\npackets <= 512 B,\nUDP / TCP per line', fs=7.5)
    box(ax, 5.0, 0.55, 1.65, 1.05, 'IP bridge\nSLIP, NAPT,\nDNS relay', fs=7.5)
    box(ax, 6.95, 0.55, 0.85, 3.95, 'Wi-Fi\n\nstation\nor\naccess\npoint', ec=C_GREEN, fs=8)
    box(ax, 8.35, 3.0, 2.1, 1.5, 'Wi-Fi network\nboat access point,\nor the module\'s own\n(phones, laptops)',
        fc=C_LIGHT, ec=C_GREY)
    box(ax, 8.35, 0.8, 2.1, 1.3, 'UDP / TCP peers\nfixed, senders\nor broadcast', fc=C_LIGHT, ec=C_GREY)
    arrow(ax, (1.85, 4.05), (2.65, 4.05), text='X1')
    arrow(ax, (1.85, 1.7), (2.65, 1.7), text='X2')
    arrow(ax, (3.9, 4.05), (4.15, 4.05))
    arrow(ax, (3.9, 1.7), (4.15, 1.7))
    for y in (3.97, 2.52, 1.07):
        arrow(ax, (4.75, y), (5.0, y))
        arrow(ax, (6.65, y), (6.95, y))
    arrow(ax, (5.8, 3.05), (5.8, 3.45))
    arrow(ax, (7.8, 3.75), (8.35, 3.75), col=C_GREEN)
    arrow(ax, (7.8, 1.45), (8.35, 1.45), col=C_GREEN)
    ax.text(0.05, -0.3, "Frames with the module's SBP address reach the manager from any line or from the network; "
            "everything else is relayed\nas whole frames. SBP and the IP bridge share line 0: the first frame after "
            "boot selects the protocol.", fontsize=7, color='#444444')
    save(fig, out, 'architecture.svg')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', default=os.path.join(ROOT, 'docs', 'img'))
    ap.add_argument('--image-bytes', type=int, default=IMAGE_BYTES, help='firmware image size for the flash map')
    ap.add_argument('--format', choices=('svg', 'png'), default='svg', help='png: for a quick look, not committed')
    a = ap.parse_args()
    global FORMAT
    FORMAT = a.format
    os.makedirs(a.out, exist_ok=True)
    style()
    chart_architecture(a.out)
    chart_uart_throughput(a.out)
    chart_rx_ring(a.out)
    chart_tx_power(a.out)
    chart_ota_timeline(a.out)
    chart_flash_map(a.out, a.image_bytes)
    chart_bridge_rtt(a.out)
    lens, wb, nb, bars = chart_rescan(a.out)
    print('rescan, false syncs: ' + ', '.join('%d KB %.1fx/%.0fx' % (n // 1024, w, x) for n, w, x in zip(lens, wb, nb)))
    print('rescan, normal: ' + ', '.join('%s %.2fx/%.2fx' % b for b in bars))


if __name__ == '__main__':
    main()
