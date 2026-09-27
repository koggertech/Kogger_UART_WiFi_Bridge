#!/usr/bin/env python3
"""Host side of the ESP32-C3 Wi-Fi bridge (Linux, run as root by espwifi.service).

- Creates TUN interface espwifi0 (10.99.0.2 <-> 10.99.0.1), default route with a high metric,
  DNS via systemd-resolved pointing at the bridge's relay.
- Moves IPv4 packets between the TUN and the serial link (framing: wbframe.py / docs/PROTOCOL.md).
- Exposes the control channel on TCP 127.0.0.1:5550 as newline-terminated lines. Each client's
  tags are made unique towards the ESP and restored on the way back; '*' events go to everyone.
- Survives unplug/replug of the ESP: the serial port is reopened every second.

Standard library only.
"""
import argparse
import errno
import fcntl
import os
import selectors
import socket
import struct
import subprocess
import sys
import termios
import time
import tty

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wbframe  # noqa: E402

TUNSETIFF = 0x400454CA
IFF_TUN, IFF_NO_PI = 0x0001, 0x1000
TIOCMBIS = 0x5416
TIOCM_DTR, TIOCM_RTS = 0x002, 0x004
MAX_SERIAL_BACKLOG = 256 * 1024


def log(*a):
    print(time.strftime('%H:%M:%S'), *a, flush=True)


def run(cmd, check=False):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        log('cmd failed (%d): %s: %s' % (r.returncode, ' '.join(cmd), r.stdout.strip()))
        if check:
            raise SystemExit(1)
    return r.returncode == 0


def open_tun(name):
    fd = os.open('/dev/net/tun', os.O_RDWR | os.O_NONBLOCK)
    fcntl.ioctl(fd, TUNSETIFF, struct.pack('16sH', name.encode(), IFF_TUN | IFF_NO_PI))
    return fd


def configure_tun(a):
    ifc = a.tun
    run(['sysctl', '-q', '-w', 'net.ipv6.conf.%s.disable_ipv6=1' % ifc])
    run(['ip', 'addr', 'flush', 'dev', ifc])
    run(['ip', 'addr', 'add', a.local, 'peer', a.peer, 'dev', ifc], check=True)
    run(['ip', 'link', 'set', ifc, 'mtu', str(wbframe.MAX_PAYLOAD), 'up'], check=True)
    run(['ip', 'route', 'replace', 'default', 'via', a.peer, 'dev', ifc, 'metric', str(a.metric)])
    if a.dns:
        run(['resolvectl', 'dns', ifc, a.peer])
        run(['resolvectl', 'domain', ifc, '~.'])
        run(['resolvectl', 'default-route', ifc, 'yes'])


def open_serial(path, baud):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    tty.setraw(fd)
    attr = termios.tcgetattr(fd)
    attr[2] |= termios.CLOCAL
    attr[2] &= ~termios.HUPCL
    attr[2] &= ~getattr(termios, 'CRTSCTS', 0)
    speed = getattr(termios, 'B%d' % baud, None)
    if speed is None:  # silently keeping the old rate would look like a dead module
        os.close(fd)
        raise SystemExit('baud %d has no termios constant on this system' % baud)
    attr[4] = attr[5] = speed  # meaningless for ttyACM, needed for USB-UART bridges
    termios.tcsetattr(fd, termios.TCSANOW, attr)
    # DTR and RTS both asserted = no reset on ESP auto-reset circuits and on USB Serial/JTAG.
    fcntl.ioctl(fd, TIOCMBIS, struct.pack('I', TIOCM_DTR | TIOCM_RTS))
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


class Client:
    def __init__(self, sock, cid):
        self.sock, self.cid = sock, cid
        self.rbuf = b''
        self.wbuf = b''


class Bridge:
    def __init__(self, a):
        self.a = a
        self.sel = selectors.DefaultSelector()
        self.tun = open_tun(a.tun)
        if not a.no_config:
            configure_tun(a)
        self.sel.register(self.tun, selectors.EVENT_READ, 'tun')
        host, port = a.ctl.rsplit(':', 1)
        self.lsock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.lsock.bind((host, int(port)))
        self.lsock.listen(4)
        self.lsock.setblocking(False)
        self.sel.register(self.lsock, selectors.EVENT_READ, 'listen')
        self.clients = {}
        self.next_cid = 1
        self.ser = None
        self.sbuf = bytearray()
        self.dec = wbframe.Decoder()
        self.next_open = 0.0
        self.stats = dict(to_esp=0, from_esp=0, dropped=0, opens=0)

    # ---- serial -------------------------------------------------------------
    def serial_try_open(self):
        if self.ser is not None or time.monotonic() < self.next_open:
            return
        self.next_open = time.monotonic() + 1.0
        try:
            self.ser = open_serial(self.a.port, self.a.baud)
        except OSError:
            return
        self.sbuf.clear()
        self.dec = wbframe.Decoder()
        self.stats['opens'] += 1
        self.sel.register(self.ser, selectors.EVENT_READ, 'serial')
        log('link up on', self.a.port)
        self.broadcast('* LINK state=UP port=%s' % wbframe.pct_encode(self.a.port))

    def serial_close(self, why):
        if self.ser is None:
            return
        self.sel.unregister(self.ser)
        os.close(self.ser)
        self.ser = None
        log('link down:', why)
        self.broadcast('* LINK state=DOWN')

    def serial_send(self, ftype, payload):
        if self.ser is None:
            self.stats['dropped'] += 1
            return False
        if ftype == wbframe.T_IP and len(self.sbuf) > MAX_SERIAL_BACKLOG:
            self.stats['dropped'] += 1
            return False
        self.sbuf += wbframe.encode(ftype, payload)
        self.serial_flush()
        return True

    def serial_flush(self):
        while self.sbuf and self.ser is not None:
            try:
                n = os.write(self.ser, self.sbuf)
            except BlockingIOError:
                break
            except OSError as e:
                self.serial_close('write: %s' % e)
                return
            del self.sbuf[:n]
        if self.ser is not None:
            ev = selectors.EVENT_READ | (selectors.EVENT_WRITE if self.sbuf else 0)
            self.sel.modify(self.ser, ev, 'serial')

    def serial_read(self):
        try:
            data = os.read(self.ser, 65536)
        except BlockingIOError:
            return
        except OSError as e:
            self.serial_close('read: %s' % e)
            return
        if not data:
            self.serial_close('EOF')
            return
        for ftype, payload in self.dec.feed(data):
            if ftype == wbframe.T_IP:
                try:
                    os.write(self.tun, payload)
                    self.stats['from_esp'] += 1
                except OSError:
                    self.stats['dropped'] += 1
            elif ftype == wbframe.T_CTL:
                self.from_esp_line(payload.decode('utf-8', 'replace'))

    # ---- control clients ----------------------------------------------------
    def broadcast(self, line):
        for c in list(self.clients.values()):
            self.client_send(c, line)

    def client_send(self, c, line):
        c.wbuf += line.encode('utf-8') + b'\n'
        self.client_flush(c)

    def client_flush(self, c):
        try:
            n = c.sock.send(c.wbuf)
            c.wbuf = c.wbuf[n:]
        except BlockingIOError:
            pass
        except OSError:
            self.client_drop(c)
            return
        if len(c.wbuf) > 1 << 20:
            self.client_drop(c)
            return
        ev = selectors.EVENT_READ | (selectors.EVENT_WRITE if c.wbuf else 0)
        self.sel.modify(c.sock, ev, c)

    def client_drop(self, c):
        if c.cid in self.clients:
            del self.clients[c.cid]
            self.sel.unregister(c.sock)
            c.sock.close()

    def from_esp_line(self, line):
        tag, _, rest = line.partition(' ')
        if tag == '*':
            self.broadcast(line)
            return
        cid, dot, orig = tag.partition('.')
        if dot and cid.startswith('c') and cid[1:].isdigit():
            c = self.clients.get(int(cid[1:]))
            if c:
                self.client_send(c, orig + ' ' + rest)
            return
        self.broadcast(line)  # untagged/unknown ('? ERR BAD_LINE' etc.)

    def client_line(self, c, line):
        line = line.strip()
        if not line:
            return
        parts = line.split()
        if len(parts) < 2 or parts[0] == '*' or len(parts[0]) > 8:
            self.client_send(c, '? ERR BAD_LINE need "<tag> <CMD> [key=value ...]", tag 1..8 chars')
            return
        tag, cmd = parts[0], parts[1]
        if cmd == 'LINK':  # answered by the daemon itself
            self.client_send(c, '%s OK state=%s to_esp=%d from_esp=%d dropped=%d opens=%d crc=%d disc=%d' % (
                tag, 'UP' if self.ser is not None else 'DOWN', self.stats['to_esp'], self.stats['from_esp'],
                self.stats['dropped'], self.stats['opens'], self.dec.crc_errors, self.dec.discarded))
            return
        if self.ser is None:
            self.client_send(c, '%s ERR LINK_DOWN' % tag)
            return
        rest = line[len(tag):].lstrip()
        self.serial_send(wbframe.T_CTL, ('c%d.%s %s' % (c.cid, tag, rest)).encode('utf-8'))

    def client_read(self, c):
        try:
            data = c.sock.recv(4096)
        except BlockingIOError:
            return
        except OSError:
            data = b''
        if not data:
            self.client_drop(c)
            return
        c.rbuf += data
        while b'\n' in c.rbuf:
            raw, c.rbuf = c.rbuf.split(b'\n', 1)
            self.client_line(c, raw.decode('utf-8', 'replace'))
        if len(c.rbuf) > 4096:
            self.client_drop(c)

    # ---- main loop ----------------------------------------------------------
    def run(self):
        log('espwifi bridge: tun=%s local=%s peer=%s port=%s ctl=%s' % (
            self.a.tun, self.a.local, self.a.peer, self.a.port, self.a.ctl))
        while True:
            self.serial_try_open()
            for key, ev in self.sel.select(timeout=0.5):
                d = key.data
                if d == 'tun':
                    self.tun_read()
                elif d == 'serial':
                    # an earlier event of this batch may have closed the port (write error on unplug)
                    if ev & selectors.EVENT_READ and self.ser is not None:
                        self.serial_read()
                    if ev & selectors.EVENT_WRITE and self.ser is not None:
                        self.serial_flush()
                elif d == 'listen':
                    s, _ = self.lsock.accept()
                    s.setblocking(False)
                    c = Client(s, self.next_cid)
                    self.next_cid += 1
                    self.clients[c.cid] = c
                    self.sel.register(s, selectors.EVENT_READ, c)
                    self.client_send(c, '* LINK state=%s' % ('UP' if self.ser is not None else 'DOWN'))
                elif isinstance(d, Client):
                    if ev & selectors.EVENT_READ:
                        self.client_read(d)
                    if ev & selectors.EVENT_WRITE and d.cid in self.clients:
                        self.client_flush(d)

    def tun_read(self):
        for _ in range(64):
            try:
                pkt = os.read(self.tun, 65536)
            except BlockingIOError:
                return
            except OSError as e:
                if e.errno == errno.EINTR:
                    continue
                raise
            if len(pkt) >= 20 and pkt[0] >> 4 == 4 and len(pkt) <= wbframe.MAX_PAYLOAD:
                if self.serial_send(wbframe.T_IP, pkt):
                    self.stats['to_esp'] += 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', default='/dev/espwifi', help='serial device (udev symlink by default)')
    ap.add_argument('--baud', type=int, default=921600, help='for USB-UART bridges; ignored by ttyACM')
    ap.add_argument('--tun', default='espwifi0')
    ap.add_argument('--local', default='10.99.0.2')
    ap.add_argument('--peer', default='10.99.0.1')
    ap.add_argument('--metric', type=int, default=700, help='default-route metric (Ethernet via NM is 100)')
    ap.add_argument('--ctl', default='127.0.0.1:5550')
    ap.add_argument('--no-dns', dest='dns', action='store_false', help='do not touch systemd-resolved')
    ap.add_argument('--no-config', action='store_true', help='do not run ip/resolvectl (testing)')
    Bridge(ap.parse_args()).run()


if __name__ == '__main__':
    main()
