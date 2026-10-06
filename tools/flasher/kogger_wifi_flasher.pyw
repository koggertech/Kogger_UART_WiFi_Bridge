#!/usr/bin/env python3
"""Flash a Kogger Wi-Fi module (ESP32-C3) in the ROM bootloader: choose the firmware file, press "Flash"
(docs/FLASHER.md). The window's language: --lang ru|en, else lang.txt packed into the .exe (make_exe.py --lang),
else Russian when lang/ru.json is present, else English.

The module is put into the bootloader by hand - hold BOOT, power it, release BOOT - and reaches the PC through a
USB-UART adapter on X1. The tool finds the port where an ESP32-C3 bootloader answers and writes, in one esptool call,
the bootloader, the partition table and the first-boot OTA data (parts/, hashes in parts/manifest.json) and the chosen
image (the .ufww of a release, or the .bin of the same build); esptool checks the hash of every region. The settings
area (NVS) is left alone: network, role, addresses and port rates are kept; a factory reset is BOOT held 10 s.

Started with a double click (pythonw, no console). Needs esptool >= 5 and pyserial. The log of the last run is
%APPDATA%\\KoggerWiFiFlasher\\flash.log.
"""
import contextlib
import hashlib
import io
import json
import os
import queue
import re
import struct
import sys
import threading
import tkinter as tk
from tkinter import filedialog, ttk

HERE = os.path.dirname(os.path.abspath(__file__))
PARTS = os.path.join(getattr(sys, '_MEIPASS', HERE), 'parts')  # packed into the .exe, or next to the script
STORE = os.path.join(os.environ.get('APPDATA', HERE), 'KoggerWiFiFlasher')
PROJECT = 'headunit_wifi_bridge'
CHIP_ESP32C3 = 5
DESC_MAGIC = 0xABCD5432
BAUDS = (460800, 115200)  # the fast rate first; the slow one when a cable or adapter cannot hold it

LANGS = os.path.join(getattr(sys, '_MEIPASS', HERE), 'lang')  # <code>.json: the window's strings, one file a language


def load_texts():
    """{code: {key: text}} of the lang/*.json present (the public tree has English only)."""
    out = {}
    for name in sorted(os.listdir(LANGS)) if os.path.isdir(LANGS) else []:
        if name.endswith('.json'):
            with open(os.path.join(LANGS, name), encoding='utf-8') as f:
                out[name[:-5]] = json.load(f)
    return out


TEXT = load_texts()


def pick_lang(argv):
    """--lang <code> on the command line, else lang.txt packed with the .exe (make_exe.py --lang), else Russian when
    lang/ru.json is there, else English."""
    if '--lang' in argv[:-1] and argv[argv.index('--lang') + 1] in TEXT:
        return argv[argv.index('--lang') + 1]
    try:
        lang = open(os.path.join(getattr(sys, '_MEIPASS', HERE), 'lang.txt'), encoding='ascii').read().strip()
    except OSError:
        lang = ''
    return lang if lang in TEXT else 'ru' if 'ru' in TEXT else 'en'


LANG = pick_lang(sys.argv)


def T(key):
    return TEXT[LANG][key]


def inspect_image(data):
    """(version) of a firmware file, or ValueError with the reason in plain words - the module's own checks."""
    if len(data) < 256 or data[0] != 0xE9:
        raise ValueError(T('not_image'))
    if struct.unpack_from('<H', data, 12)[0] != CHIP_ESP32C3:
        raise ValueError(T('other_chip'))
    d = 24 + 8
    if struct.unpack_from('<I', data, d)[0] != DESC_MAGIC:
        raise ValueError(T('no_desc'))
    version = data[d + 16:d + 48].split(b'\0')[0].decode(errors='replace')
    project = data[d + 48:d + 80].split(b'\0')[0].decode(errors='replace')
    if project != PROJECT:
        raise ValueError(T('other_project') % project)
    if data[23] != 1 or hashlib.sha256(data[:-32]).digest() != data[-32:]:
        raise ValueError(T('damaged'))
    return version


def load_parts():
    """Manifest of the parts and their paths, every hash checked; ValueError when the install is broken."""
    try:
        man = json.load(open(os.path.join(PARTS, 'manifest.json'), encoding='utf-8'))
    except (OSError, ValueError):
        raise ValueError(T('no_parts'))
    files = []
    for name, f in man['files'].items():
        path = os.path.join(PARTS, name)
        try:
            ok = hashlib.sha256(open(path, 'rb').read()).hexdigest() == f['sha256']
        except OSError:
            ok = False
        if not ok:
            raise ValueError(T('part_bad') % name)
        files.append((f['offset'], path))
    return man, files


def candidate_ports():
    """Serial ports, the likely ones first: a CP210x enhanced port (X1 of the bench adapter), then other USB-UARTs.
    Bluetooth ports are left out: opening one can hang for seconds."""
    from serial.tools import list_ports
    ports = [p for p in list_ports.comports() if 'bluetooth' not in (p.description or '').lower()]

    def rank(p):
        d = (p.description or '').lower()
        return (0 if 'enhanced' in d else 1 if any(k in d for k in ('cp210', 'ch34', 'ftdi', 'usb')) else 2, p.device)
    return [p.device for p in sorted(ports, key=rank)]


def percent(text):
    """The last percentage in a chunk of esptool output, or None."""
    m = re.findall(r'(\d{1,3}(?:\.\d+)?)\s*%', text)
    return float(m[-1]) if m else None


class QueueWriter(io.TextIOBase):
    def __init__(self, q, log):
        self.q, self.log = q, log

    def write(self, s):
        self.log.write(s)
        self.q.put(('out', s))
        return len(s)

    def flush(self):
        self.log.flush()


def run_esptool(argv, q, log):
    """esptool in this process (works in a packed .exe too); returns (ok, its whole output)."""
    import esptool
    buf = io.StringIO()

    class Tee(QueueWriter):
        def write(self, s):
            buf.write(s)
            return super().write(s)
    w = Tee(q, log)
    ok = True
    with contextlib.redirect_stdout(w), contextlib.redirect_stderr(w):
        try:
            esptool.main(argv)
        except SystemExit as e:
            ok = e.code in (0, None)
        except Exception as e:  # noqa: BLE001 - esptool's FatalError and serial errors alike
            w.write('\nERROR: %s\n' % e)
            ok = False
    return ok, buf.getvalue()


def flash(app_path, q):
    """Worker thread: find the port with an ESP32-C3 bootloader and write everything. Posts ('done', ok, message,
    code) with code ok / no_ports / not_found / broken / error."""
    os.makedirs(STORE, exist_ok=True)
    log = open(os.path.join(STORE, 'flash.log'), 'w', encoding='utf-8')
    try:
        man, parts = load_parts()
        regions = []
        for off, path in sorted(parts + [(man['app_offset'], app_path)], key=lambda x: int(x[0], 16)):
            regions += [off, path]
        ports = candidate_ports()
        if not ports:
            q.put(('done', False, T('no_ports'), 'no_ports'))
            return
        for port in ports:
            for baud in BAUDS:
                q.put(('status', T('searching') % port))
                argv = ['--chip', 'esp32c3', '--port', port, '--baud', str(baud), '--before', 'no-reset',
                        '--after', 'no-reset', '--connect-attempts', '2', 'write-flash',
                        '--flash-mode', man['flash']['mode'], '--flash-freq', man['flash']['freq'],
                        '--flash-size', man['flash']['size']] + regions
                ok, out = run_esptool(argv, q, log)
                if ok and out.count('Hash of data verified') >= len(regions) // 2:
                    mac = re.search(r'MAC:\s*([0-9a-f:]{17})', out)
                    where = '%s, %s%s' % (os.path.basename(app_path), port, ', MAC ' + mac.group(1) if mac else '')
                    q.put(('done', True, T('done_ok') % where, 'ok'))
                    return
                if 'Failed to connect' in out or 'No serial data received' in out or 'could not open port' in out:
                    break  # no bootloader on this port: the next one (a slower rate would not help)
                q.put(('status', T('retry_slow') % port))
        q.put(('done', False, T('not_found'), 'not_found'))
    except ValueError as e:
        q.put(('done', False, T('broken') % e, 'broken'))
    except Exception as e:  # noqa: BLE001
        q.put(('done', False, T('error') % e, 'error'))
    finally:
        log.close()


class App:
    def __init__(self, root):
        self.root, self.q, self.path, self.busy = root, queue.Queue(), None, False
        root.title(T('title'))
        root.resizable(False, False)
        f = ttk.Frame(root, padding=14)
        f.grid()
        ttk.Label(f, text=T('hint'),
                  foreground='#555').grid(row=0, column=0, columnspan=2, sticky='w', pady=(0, 10))
        self.file = tk.StringVar()
        e = ttk.Entry(f, textvariable=self.file, width=58, state='readonly')
        e.grid(row=1, column=0, sticky='we')
        e.bind('<Button-1>', lambda _: self.choose())
        ttk.Button(f, text='…', width=3, command=self.choose).grid(row=1, column=1, padx=(6, 0))
        self.info = ttk.Label(f, text=T('choose'), foreground='#555')
        self.info.grid(row=2, column=0, columnspan=2, sticky='w', pady=(4, 10))
        self.btn = ttk.Button(f, text=T('flash'), command=self.start, state='disabled')
        self.btn.grid(row=3, column=0, columnspan=2, sticky='we', ipady=6)
        self.bar = ttk.Progressbar(f, maximum=100, length=420)
        self.bar.grid(row=4, column=0, columnspan=2, sticky='we', pady=(10, 4))
        self.status = ttk.Label(f, text='', wraplength=440, justify='left')
        self.status.grid(row=5, column=0, columnspan=2, sticky='w')
        last = self.settings().get('last_file')
        if last and os.path.isfile(last):
            self.use(last)
        root.after(100, self.poll)

    def settings(self):
        try:
            return json.load(open(os.path.join(STORE, 'settings.json'), encoding='utf-8'))
        except (OSError, ValueError):
            return {}

    def choose(self):
        if self.busy:
            return
        start = os.path.dirname(self.path) if self.path else os.path.expanduser('~')
        p = filedialog.askopenfilename(title=T('dlg_title'), initialdir=start,
                                       filetypes=[(T('dlg_type'), '*.ufww *.bin'), (T('dlg_all'), '*.*')])
        if p:
            self.use(p)

    def use(self, p):
        self.file.set(p)
        try:
            version = inspect_image(open(p, 'rb').read())
        except (OSError, ValueError) as e:
            self.path = None
            self.info.config(text=T('unsuitable') % e, foreground='#b00020')
            self.btn.config(state='disabled')
            return
        self.path = p
        self.info.config(text=T('version') % version, foreground='#1b5e20')
        self.btn.config(state='normal')
        try:
            os.makedirs(STORE, exist_ok=True)
            json.dump({'last_file': p}, open(os.path.join(STORE, 'settings.json'), 'w', encoding='utf-8'))
        except OSError:
            pass

    def start(self):
        if not self.path or self.busy:
            return
        self.busy = True
        self.btn.config(state='disabled')
        self.bar['value'] = 0
        self.status.config(text=T('starting'), foreground='')
        threading.Thread(target=flash, args=(self.path, self.q), daemon=True).start()

    def poll(self):
        try:
            while True:
                kind, *rest = self.q.get_nowait()
                if kind == 'out':
                    pc = percent(rest[0])
                    if pc is not None:
                        self.bar['value'] = pc
                        self.status.config(text=T('writing') % pc, foreground='')
                elif kind == 'status':
                    self.status.config(text=rest[0], foreground='')
                elif kind == 'done':
                    ok, msg = rest[0], rest[1]
                    self.busy = False
                    self.btn.config(state='normal' if self.path else 'disabled')
                    self.bar['value'] = 100 if ok else 0
                    self.status.config(text=msg, foreground='#1b5e20' if ok else '#b00020')
        except queue.Empty:
            pass
        self.root.after(100, self.poll)


def selftest(app_path, report):
    """A check of a packed copy without a window or a module (tools/flasher/make_exe.py runs it): the file check,
    the parts, esptool's stub data for the ESP32-C3, Tk, and the 'no port' and 'not found' paths with esptool really
    run. Writes a JSON report; returns 0 when everything passed."""
    global candidate_ports
    res = {'lang': LANG, 'title': T('title')}
    try:
        res['version'] = inspect_image(open(app_path, 'rb').read())
    except (OSError, ValueError) as e:
        res['version'] = None
        res['version_error'] = str(e)
    try:
        res['parts'] = len(load_parts()[1])
    except ValueError as e:
        res['parts'] = 0
        res['parts_error'] = str(e)
    import esptool
    stub = os.path.join(os.path.dirname(esptool.__file__), 'targets', 'stub_flasher')
    res['stub_esp32c3'] = all(os.path.isfile(os.path.join(stub, v, 'esp32c3.json')) for v in ('1', '2'))
    try:
        r = tk.Tk()
        r.withdraw()
        App(r)
        r.update()
        r.destroy()
        res['window'] = True
    except Exception as e:  # noqa: BLE001
        res['window'] = False
        res['window_error'] = str(e)
    real = candidate_ports
    for name, ports in (('no_ports', []), ('not_found', ['COM199'])):
        q = queue.Queue()
        candidate_ports = lambda p=ports: p  # noqa: E731
        flash(app_path, q)
        done = []
        while not q.empty():
            m = q.get_nowait()
            if m[0] == 'done':
                done.append(m)
        res[name] = bool(done) and done[0][1] is False and done[0][3] == name
    candidate_ports = real
    res['ok'] = bool(res['version'] and res['parts'] == 3 and res['stub_esp32c3'] and res['window']
                     and res['no_ports'] and res['not_found'])
    with open(report, 'w', encoding='utf-8') as f:
        json.dump(res, f, indent=1)
    return 0 if res['ok'] else 1


def main():
    if len(sys.argv) >= 4 and sys.argv[1] == '--selftest':  # --selftest <ufww> <report.json> [--lang ru|en]
        sys.exit(selftest(sys.argv[2], sys.argv[3]))
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == '__main__':
    main()
