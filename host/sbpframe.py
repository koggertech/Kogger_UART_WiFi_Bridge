"""Kogger SBP (KP1) framing, Python mirror of firmware/main/sbp.c.

Frame: BB 55 | route | mode | id | len | payload | ck1 ck2, Fletcher-8 over route..payload,
mode = type | ver << 3 | mark << 6 | resp << 7 (as in KoggerApp ProtoBinOut). Standard library only.
Contract of the Wi-Fi module: docs/SBP_WIFI.md.
"""
import struct

CONTENT, SETTING, GETTING = 1, 2, 3
KEY_CONFIRM = 0xC96B5D4A
RESP = {1: 'OK', 2: 'ERR_CHECK', 3: 'ERR_PAYLOAD', 4: 'ERR_ID', 5: 'ERR_VERSION', 6: 'ERR_TYPE', 7: 'ERR_KEY',
        8: 'ERR_RUNTIME'}

ID_UART, ID_VERSION, ID_MARK, ID_FLASH, ID_BOOT, ID_WIFI = 0x18, 0x20, 0x21, 0x23, 0x24, 0x57
ID_WIFI_NET = 0x58
BOARD_WIFI = 87      # board number of the module in ID_VERSION
BOARD_WIFI = 87


def fletcher(data):
    c1 = c2 = 0
    for b in data:
        c1 = (c1 + b) & 0xFF
        c2 = (c2 + c1) & 0xFF
    return c1, c2


def mode(ftype, ver, resp=False, mark=False):
    return (ftype & 3) | ((ver & 7) << 3) | (int(bool(mark)) << 6) | (int(bool(resp)) << 7)


def encode(route, mode_, id_, payload=b''):
    payload = bytes(payload)
    if len(payload) > 255:
        raise ValueError('KP1 payload is at most 255 bytes')
    body = bytes([route, mode_, id_, len(payload)]) + payload
    return b'\xbb\x55' + body + bytes(fletcher(body))


class Frame:
    __slots__ = ('route', 'mode', 'id', 'payload', 'ck')

    def __init__(self, route, mode_, id_, payload, ck):
        self.route, self.mode, self.id, self.payload, self.ck = route, mode_, id_, payload, ck

    type = property(lambda s: s.mode & 3)
    ver = property(lambda s: (s.mode >> 3) & 7)
    mark = property(lambda s: bool(s.mode & 0x40))
    resp = property(lambda s: bool(s.mode & 0x80))

    def __repr__(self):
        return 'Frame(route=%d type=%d ver=%d resp=%d mark=%d id=0x%02X len=%d)' % (
            self.route, self.type, self.ver, self.resp, self.mark, self.id, len(self.payload))


class Decoder:
    """Byte-for-byte the state machine of sbp_dec_feed()."""

    def __init__(self):
        self.st = 0
        self.frames_ok = self.check_errors = 0
        self._hdr = []
        self._pl = bytearray()
        self._ck1 = 0

    def feed(self, data):
        out = []
        for b in data:
            st = self.st
            if st == 0:
                if b == 0xBB:
                    self.st = 1
            elif st == 1:
                self.st = 2 if b == 0x55 else (1 if b == 0xBB else 0)
            elif st in (2, 3, 4):
                if st == 2:
                    self._hdr = []
                self._hdr.append(b)
                self.st = st + 1
            elif st == 5:
                self._hdr.append(b)
                self._pl = bytearray()
                self.st = 6 if b else 7
            elif st == 6:
                self._pl.append(b)
                if len(self._pl) >= self._hdr[3]:
                    self.st = 7
            elif st == 7:
                self._ck1 = b
                self.st = 8
            else:
                self.st = 0
                c1, c2 = fletcher(bytes(self._hdr) + bytes(self._pl))
                if self._ck1 == c1 and b == c2:
                    self.frames_ok += 1
                    r, m, i, _ = self._hdr
                    out.append(Frame(r, m, i, bytes(self._pl), (self._ck1, b)))
                else:
                    self.check_errors += 1
        return out


# ---- ID_WIFI payload helpers (docs/SBP_WIFI.md) ---------------------------------------------

STATES = ['IDLE', 'SEARCHING', 'CONNECTING', 'CONNECTED', 'AP']
# ESP-IDF esp_reset_reason_t, sent at the end of ID_WIFI v1 since 0.11
RESET_REASONS = ['UNKNOWN', 'POWERON', 'EXT', 'SW', 'PANIC', 'INT_WDT', 'TASK_WDT', 'WDT', 'DEEPSLEEP',
                 'BROWNOUT', 'SDIO', 'USB', 'JTAG', 'EFUSE', 'PWR_GLITCH', 'CPU_LOCKUP']
AUTHS = ['OPEN', 'WEP', 'WPA', 'WPA2', 'WPA/WPA2', 'WPA3', 'WPA2/WPA3', 'ENTERPRISE', 'OWE']
WHYS = ['NONE', 'NO_AP', 'AUTH', 'ASSOC', 'LOST', 'OTHER']


def _ip(b):
    return '.'.join(str(x) for x in b)


def _auth(c):
    return AUTHS[c] if c < len(AUTHS) else ('?' if c == 255 else 'code%d' % c)


def parse_status(p):
    """ID_WIFI CONTENT v0."""
    if len(p) < 39 or len(p) < 39 + p[38]:
        raise ValueError('bad v0 length %d' % len(p))
    return dict(state=STATES[p[0]] if p[0] < len(STATES) else p[0], auto=bool(p[1] & 1), scanning=bool(p[1] & 2),
                saved=bool(p[1] & 4), rssi=struct.unpack('b', p[2:3])[0], ch=p[3], auth=_auth(p[4]),
                reason=p[5], why=WHYS[p[6]] if p[6] < len(WHYS) else p[6], clients=p[7],
                bssid=':'.join('%02x' % x for x in p[8:14]),
                ip=_ip(p[14:18]), mask=_ip(p[18:22]), gw=_ip(p[22:26]), dns=_ip(p[26:30]),
                rx_bps=struct.unpack('<I', p[30:34])[0], tx_bps=struct.unpack('<I', p[34:38])[0],
                ssid=p[39:39 + p[38]].decode('utf-8', 'replace'))


def parse_link(p):
    """ID_WIFI CONTENT v1 (22 bytes; 0.11 appends the reset reason)."""
    if len(p) < 22:
        raise ValueError('bad v1 length %d' % len(p))
    st, rssi = p[0], struct.unpack('b', p[1:2])[0]
    rx_bps, tx_bps, rx_tot, tx_tot, up = struct.unpack('<5I', p[2:22])
    d = dict(state=STATES[st] if st < len(STATES) else st, rssi=rssi, rx_bps=rx_bps, tx_bps=tx_bps, rx_total=rx_tot,
             tx_total=tx_tot, uptime=up)
    if len(p) > 22:
        d['reset'] = RESET_REASONS[p[22]] if p[22] < len(RESET_REASONS) else p[22]
    return d


def parse_net(p):
    """ID_WIFI CONTENT v2: one scan result (or {index 0, total 0} = none found)."""
    if len(p) == 2:
        return dict(index=p[0], total=p[1])
    if len(p) < 13 or len(p) < 13 + p[12]:
        raise ValueError('bad v2 length %d' % len(p))
    return dict(index=p[0], total=p[1], rssi=struct.unpack('b', p[2:3])[0], ch=p[3], auth=_auth(p[4]),
                saved=bool(p[5] & 1), current=bool(p[5] & 2), bssid=':'.join('%02x' % x for x in p[6:12]),
                ssid=p[13:13 + p[12]].decode('utf-8', 'replace'))


def parse_saved(p):
    """ID_WIFI CONTENT v4: one saved network (or {0, 0})."""
    if len(p) == 2:
        return dict(index=p[0], total=p[1])
    if len(p) < 3 or len(p) < 3 + p[2]:
        raise ValueError('bad v4 length %d' % len(p))
    return dict(index=p[0], total=p[1], ssid=p[3:3 + p[2]].decode('utf-8', 'replace'))


RADIO_MODES = {0x01: 'b', 0x03: 'bg', 0x07: 'bgn', 0x08: 'lr', 0x0F: 'bgnlr'}
PHY_MODES = {0: 'LR', 1: '11b', 2: '11g', 4: 'HT20', 5: 'HT40', 0xFF: 'none'}


def parse_radio(p):
    """ID_WIFI CONTENT v7: settings, then what the driver holds (proto_now/bw_now are the interface's
    settings, not negotiated; the negotiated mode is `phy`)."""
    if len(p) < 8:
        raise ValueError('bad v7 length %d' % len(p))
    return dict(power_dbm=p[0] / 4, mode=RADIO_MODES.get(p[1], hex(p[1])), bw=40 if p[2] == 2 else 20,
                ps=['none', 'min', 'max'][p[3]] if p[3] < 3 else p[3], power_now_dbm=p[4] / 4,
                phy=PHY_MODES.get(p[5], p[5]), proto_now=p[6], bw_now=40 if p[7] == 2 else 20)


def radio_payload(power_dbm, mode='bgn', bw=20, ps='none'):
    code = {v: k for k, v in RADIO_MODES.items()}[mode]
    return bytes([int(round(power_dbm * 4)), code, 2 if bw == 40 else 1, ['none', 'min', 'max'].index(ps)])


def connect_payload(ssid, password='', save=True):
    s = ssid.encode('utf-8') if isinstance(ssid, str) else bytes(ssid)
    pw = password.encode('utf-8') if isinstance(password, str) else bytes(password)
    return bytes([1 if save else 0, len(s)]) + s + bytes([len(pw)]) + pw


# ---- ID_WIFI_NET payload helpers (docs/SBP_WIFI.md); every SETTING starts with the key ----------

KEY = KEY_CONFIRM.to_bytes(4, 'little')
ROLES = ['sta', 'ap']
AP_AUTHS = ['open', 'wpa2', 'wpa2wpa3']
LINE_MODES = ['off', 'udp', 'tcp']
LINE_DESTS = ['fixed', 'senders', 'broadcast']
LINE_STATES = ['off', 'no_wifi', 'no_peer', 'ready']


def _ipb(s):
    b = bytes(int(x) for x in s.split('.'))
    if len(b) != 4:
        raise ValueError('bad address %r' % s)
    return b


def _i8(b):
    return b - 256 if b > 127 else b


def parse_role(p):
    """ID_WIFI_NET CONTENT v0."""
    if len(p) < 2:
        raise ValueError('bad v0 length %d' % len(p))
    return dict(saved=ROLES[p[0]] if p[0] < 2 else p[0], running=ROLES[p[1]] if p[1] < 2 else p[1])


def role_payload(role, reboot=False, forget_lines=False):
    return KEY + bytes([ROLES.index(role), (1 if reboot else 0) | (2 if forget_lines else 0)])


def parse_ap(p):
    """ID_WIFI_NET CONTENT v1 (the password is never read back: its length is 0)."""
    if len(p) < 6 or len(p) < 6 + p[4] or len(p) < 6 + p[4] + p[5 + p[4]]:
        raise ValueError('bad v1 length %d' % len(p))
    return dict(channel=p[0], hidden=bool(p[1]), max_clients=p[2], auth=AP_AUTHS[p[3]] if p[3] < 3 else p[3],
                ssid=p[5:5 + p[4]].decode('utf-8', 'replace'))


def ap_payload(ssid, password='', channel=6, auth='wpa2', hidden=False, max_clients=4):
    """password '' with security on keeps the current one."""
    s = ssid.encode('utf-8')
    pw = password.encode('utf-8')
    return (KEY + bytes([channel, 1 if hidden else 0, max_clients, AP_AUTHS.index(auth), len(s)]) + s +
            bytes([len(pw)]) + pw)


def parse_ipcfg(p):
    """ID_WIFI_NET CONTENT v2."""
    if len(p) < 20:
        raise ValueError('bad v2 length %d' % len(p))
    return dict(ip=_ip(p[0:4]), mask=_ip(p[4:8]), dhcp=bool(p[8]), pool_start=_ip(p[9:13]), pool_end=_ip(p[13:17]),
                lease_min=struct.unpack('<H', p[17:19])[0], offer_gw=bool(p[19] & 1), offer_dns=bool(p[19] & 2))


def ipcfg_payload(ip, mask='255.255.255.0', dhcp=True, pool_start='0.0.0.0', pool_end='0.0.0.0', lease_min=120,
                  offer_gw=False, offer_dns=False):
    return (KEY + _ipb(ip) + _ipb(mask) + bytes([1 if dhcp else 0]) + _ipb(pool_start) + _ipb(pool_end) +
            struct.pack('<H', lease_min) + bytes([(1 if offer_gw else 0) | (2 if offer_dns else 0)]))


def parse_line(p):
    """ID_WIFI_NET CONTENT v3: one line."""
    if len(p) < 18:
        raise ValueError('bad v3 length %d' % len(p))
    rport, lport, baud = struct.unpack('<HHI', p[7:15])
    return dict(line=p[0], mode=LINE_MODES[p[1]] if p[1] < 3 else p[1], dest=LINE_DESTS[p[2]] if p[2] < 3 else p[2],
                ip=_ip(p[3:7]), rport=rport, lport=lport, baud=baud, tx_pin=_i8(p[15]), rx_pin=_i8(p[16]),
                uart=['off', 'running', 'next_boot'][p[17]] if p[17] < 3 else p[17])


def line_payload(line, mode='udp', dest='senders', ip='0.0.0.0', rport=0, lport=0, baud=115200, tx_pin=-1, rx_pin=-1):
    return (KEY + bytes([line, LINE_MODES.index(mode), LINE_DESTS.index(dest)]) + _ipb(ip) +
            struct.pack('<HHIbb', rport, lport, baud, tx_pin, rx_pin))


def line_payload_from(d, **change):
    """SETTING payload from a parse_line() result with some fields changed."""
    x = dict(d)
    x.update(change)
    return line_payload(x['line'], x['mode'], x['dest'], x['ip'], x['rport'], x['lport'], x['baud'], x['tx_pin'],
                        x['rx_pin'])


LINE_COUNTERS = ['up_frames', 'up_bytes', 'up_packets', 'up_drops', 'down_packets', 'down_bytes', 'down_frames',
                 'down_drops', 'local_frames', 'tcp_connects', 'uart_overflows', 'uart_drops']


def parse_line_stats(p):
    """ID_WIFI_NET CONTENT v4: one line."""
    if len(p) < 57:
        raise ValueError('bad v4 length %d' % len(p))
    d = dict(line=p[0], state=LINE_STATES[p[1]] if p[1] < 4 else p[1],
             peer='%s:%d' % (_ip(p[2:6]), struct.unpack('<H', p[6:8])[0]), npeers=p[8])
    d.update(zip(LINE_COUNTERS, struct.unpack('<12I', p[9:57])))
    return d


def parse_client(p):
    """ID_WIFI_NET CONTENT v5: one access point client (or {0, 0})."""
    if len(p) == 2:
        return dict(index=p[0], total=p[1])
    if len(p) < 13:
        raise ValueError('bad v5 length %d' % len(p))
    return dict(index=p[0], total=p[1], mac=':'.join('%02x' % x for x in p[2:8]), ip=_ip(p[8:12]), rssi=_i8(p[12]))


# ---- 0.12: discovery and ID_WIFI_NET v7 port information (docs/SBP_WIFI.md) --------------------

ROUTE_BCAST0, ROUTE_BCAST = 0, 255
PORT_NAMES = ['X1', 'X2']
PORT_PROTOS = ['kp1', 'kp2', 'ubx', 'mav1', 'mav2', 'raw']
PORT_CONNECTED = ['nothing', 'unreadable', 'sbp_host', 'sbp_devices', 'sbp_host_devices', 'mavlink', 'ublox', 'slip',
                  'other']
PORT_FLAGS = ['uart', 'bridging', 'asked_here', 'reports', 'slip', 'provisional', 'usb']


def is_discovery(route, mode_, id_):
    """GETTING ID_VERSION to route 0 or 255: the module answers from its own address (and relays it)."""
    return route in (ROUTE_BCAST0, ROUTE_BCAST) and (mode_ & 3) == GETTING and id_ == ID_VERSION


def ports_payload(port=None, page=None):
    """GETTING ID_WIFI_NET v7: none = page 0 of both ports, (port) = its page 0, (port, page)."""
    if port is None:
        return b''
    return bytes([port]) if page is None else bytes([port, page])


def _age(v):
    return None if v == 0xFFFF else v / 10.0


def parse_port(p):
    """ID_WIFI_NET CONTENT v7: one page of one port."""
    if len(p) < 2:
        raise ValueError('bad v7 length %d' % len(p))
    port, page = p[0], p[1]
    if page == 0:
        if len(p) < 116:
            raise ValueError('bad v7 page 0 length %d' % len(p))
        d = dict(port=port, page=0, flags={n: bool(p[2] >> i & 1) for i, n in enumerate(PORT_FLAGS)},
                 baud=struct.unpack('<I', p[3:7])[0], saved=struct.unpack('<I', p[7:11])[0],
                 connected=PORT_CONNECTED[p[11]] if p[11] < len(PORT_CONNECTED) else p[11])
        d['rx_bytes'], d['tx_bytes'], ra, ta = struct.unpack('<IIHH', p[12:24])
        d['rx_age'], d['tx_age'] = _age(ra), _age(ta)
        d['protos'] = {}
        for i, n in enumerate(PORT_PROTOS):
            up, down, age = struct.unpack('<IIH', p[24 + 10 * i:34 + 10 * i])
            d['protos'][n] = dict(up=up, down=down, age=_age(age))
        names = ['sbp_req_up', 'sbp_content_up', 'sbp_req_down', 'sbp_content_down', 'module_rx', 'module_tx',
                 'rx_overflows', 'tx_drops']
        d.update(zip(names, struct.unpack('<8I', p[84:116])))
        return d
    if page == 1:
        n = p[2] if len(p) > 2 else 0
        if len(p) < 3 + 12 * n:
            raise ValueError('bad v7 page 1 length %d' % len(p))
        devs = []
        for i in range(n):
            e = p[3 + 12 * i:15 + 12 * i]
            age, v = struct.unpack('<HI', e[6:12])
            if e[0] == 1:
                devs.append(dict(kind='sbp', addr=e[1], board=e[2], fw=(e[3], e[4]), version_known=bool(e[5] & 1),
                                 age=_age(age), serial=v))
            else:
                devs.append(dict(kind='mavlink', sysid=e[1], compid=e[2], mav_type=e[3], autopilot=e[4],
                                 heartbeat=bool(e[5] & 2), age=_age(age), frames=v))
        return dict(port=port, page=1, devices=devs)
    if page == 2:
        n = p[4] if len(p) > 4 else 0
        if len(p) < 5 + 8 * n:
            raise ValueError('bad v7 page 2 length %d' % len(p))
        peers = []
        for i in range(n):
            e = p[5 + 8 * i:13 + 8 * i]
            pt, age = struct.unpack('<HH', e[4:8])
            peers.append(dict(ip=_ip(e[0:4]), port=pt, age=_age(age)))
        return dict(port=port, page=2, mode=p[2], state=LINE_STATES[p[3]] if p[3] < 4 else p[3], peers=peers)
    raise ValueError('unknown v7 page %d' % page)
