#include "netctl.h"

#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "link.h"
#include "netcfg.h"
#include "portinfo.h"
#include "ports.h"
#include "relay.h"
#include "sbpdev.h"
#include "uline.h"
#include "wifiap.h"

static const char *TAG = "netctl";

enum { V_ROLE = 0, V_AP, V_IP, V_LINE, V_STATS, V_CLIENTS, V_ADDR, V_PORTS };

#define ROLE_F_REBOOT 1u   /* reboot right after the answer */
#define ROLE_F_FORGET 2u   /* forget saved lines and own address: the new role starts from its defaults */
#define LINE_REC      17   /* line, mode, dest, ip[4], rport, lport, baud, tx_pin, rx_pin */

/* A change that restarts the radio or the module waits until its answer is on the air: a peer that
 * talks through the access point would otherwise never see it. */
static void settle(void)
{
    vTaskDelay(pdMS_TO_TICKS(200));
}

static void send(uint8_t ver, const uint8_t *p, size_t n)
{
    sbpdev_send(SBP_T_CONTENT, ver, SBP_ID_WIFI_NET, p, (uint8_t)n);
}

static bool running_ap(void)
{
    return netcfg_role() == ROLE_AP;
}

/* ---- v0 role ---------------------------------------------------------------------------------------- */

static void send_role(void)
{
    const uint8_t q[2] = { (uint8_t)netcfg_saved_role(), (uint8_t)netcfg_role() };
    send(V_ROLE, q, sizeof q);
}

static void set_role(const sbp_frame_t *f)
{
    const uint8_t *p = f->payload;
    if (f->len != 6 || p[4] > ROLE_AP || (p[5] & ~(ROLE_F_REBOOT | ROLE_F_FORGET))) {
        sbpdev_ack(f, SBP_RESP_ERR_PAYLOAD);
        return;
    }
    bool ok = netcfg_set_role((wifi_role_t)p[4]) && (!(p[5] & ROLE_F_FORGET) || netcfg_forget_lines());
    sbpdev_ack(f, ok ? SBP_RESP_OK : SBP_RESP_ERR_RUNTIME);
    send_role();
    if (ok && (p[5] & ROLE_F_REBOOT)) {
        ESP_LOGI(TAG, "role %u: rebooting", p[4]);
        sbpdev_save_resume(); /* the host keeps talking at the current rate and address */
        settle();
        esp_restart();
    }
}

/* ---- v1 access point -------------------------------------------------------------------------------- */

static void send_ap(void)
{
    ap_cfg_t a;
    netcfg_ap(&a);
    uint8_t q[6 + 32];
    size_t sl = strnlen(a.ssid, 32);
    q[0] = a.channel;
    q[1] = a.hidden;
    q[2] = a.max_clients;
    q[3] = a.auth;
    q[4] = (uint8_t)sl;
    memcpy(q + 5, a.ssid, sl);
    q[5 + sl] = 0; /* the password is never read back */
    send(V_AP, q, 6 + sl);
}

static void set_ap(const sbp_frame_t *f)
{
    const uint8_t *p = f->payload + 4;
    size_t n = f->len - 4u;
    ap_cfg_t a, old;
    netcfg_ap(&a);
    old = a;
    uint8_t sl = n >= 5 ? p[4] : 0;
    uint8_t pl = n >= 6u + sl ? p[5 + sl] : 0xFF;
    if (sl < 1 || sl > 32 || pl > 63 || n != 6u + sl + pl || memchr(p + 5, 0, sl) || memchr(p + 6 + sl, 0, pl)) {
        sbpdev_ack(f, SBP_RESP_ERR_PAYLOAD);
        return;
    }
    a.channel = p[0];
    a.hidden = p[1];
    a.max_clients = p[2];
    a.auth = p[3];
    memset(a.ssid, 0, sizeof a.ssid);
    memcpy(a.ssid, p + 5, sl);
    if (a.auth == 0) {
        memset(a.pass, 0, sizeof a.pass); /* an open network has none */
    } else if (pl) {                      /* none given with security on: the current one stays */
        memset(a.pass, 0, sizeof a.pass);
        memcpy(a.pass, p + 6 + sl, pl);
    }
    if (!netcfg_ap_ok(&a)) {
        sbpdev_ack(f, SBP_RESP_ERR_PAYLOAD);
        return;
    }
    bool ok = netcfg_set_ap(&a);
    sbpdev_ack(f, ok ? SBP_RESP_OK : SBP_RESP_ERR_RUNTIME);
    send_ap();
    if (ok && running_ap()) {
        settle();
        if (!wifiap_apply_ap(&a)) { /* the driver refused: back to what ran, in NVS and on the air */
            ESP_LOGW(TAG, "access point settings refused by the driver: previous ones restored");
            netcfg_set_ap(&old);
            wifiap_apply_ap(&old);
        }
    }
}

/* ---- v2 address and DHCP server -------------------------------------------------------------------- */

static void send_ip(void)
{
    ip_cfg_t c;
    netcfg_ip(&c);
    uint8_t q[20];
    memcpy(q, c.ip, 4);
    memcpy(q + 4, c.mask, 4);
    q[8] = c.dhcp;
    memcpy(q + 9, c.pool_start, 4);
    memcpy(q + 13, c.pool_end, 4);
    sbp_put_u16(q + 17, c.lease_min);
    q[19] = c.offer;
    send(V_IP, q, sizeof q);
}

static void set_ip(const sbp_frame_t *f)
{
    const uint8_t *p = f->payload + 4;
    if (f->len != 24) {
        sbpdev_ack(f, SBP_RESP_ERR_PAYLOAD);
        return;
    }
    ip_cfg_t c;
    memcpy(c.ip, p, 4);
    memcpy(c.mask, p + 4, 4);
    c.dhcp = p[8];
    memcpy(c.pool_start, p + 9, 4);
    memcpy(c.pool_end, p + 13, 4);
    c.lease_min = sbp_get_u16(p + 17);
    c.offer = p[19];
    if (!netcfg_ip_ok(&c)) {
        sbpdev_ack(f, SBP_RESP_ERR_PAYLOAD);
        return;
    }
    bool ok = netcfg_set_ip(&c);
    sbpdev_ack(f, ok ? SBP_RESP_OK : SBP_RESP_ERR_RUNTIME);
    send_ip();
    if (ok && running_ap()) {
        settle();
        wifiap_apply_ip(&c);
    }
}

/* ---- v3 lines --------------------------------------------------------------------------------------- */

/* The rate shown is the running one (ports.c; the saved one while a UART does not run). Line 0's pins
 * belong to the build. */
static void line_view(int line, line_cfg_t *c)
{
    netcfg_line(line, c);
    uint32_t b = ports_baud(line);
    if (b)
        c->baud = b;
    if (line == 0) {
#if CONFIG_WB_LINK_UART
        c->tx_pin = CONFIG_WB_LINK_UART_TX;
        c->rx_pin = CONFIG_WB_LINK_UART_RX;
#else
        c->tx_pin = c->rx_pin = -1; /* USB Serial/JTAG */
#endif
    }
}

/* 0 UART off (and stays off), 1 runs on the reported pins, 2 the next boot changes it (other pins,
 * or off while it still runs). */
static uint8_t uart_state(int line, const line_cfg_t *c)
{
    if (line == 0)
        return 1;
    int8_t tx, rx;
    uline_pins(&tx, &rx);
    if (uline_running())
        return tx == c->tx_pin && rx == c->rx_pin ? 1 : 2;
    return c->tx_pin >= 0 && c->rx_pin >= 0 ? 2 : 0;
}

static void send_line(int line)
{
    line_cfg_t c;
    line_view(line, &c);
    uint8_t q[LINE_REC + 1];
    q[0] = (uint8_t)line;
    q[1] = c.mode;
    q[2] = c.dest;
    memcpy(q + 3, c.ip, 4);
    sbp_put_u16(q + 7, c.rport);
    sbp_put_u16(q + 9, c.lport);
    sbp_put_u32(q + 11, c.baud);
    q[15] = (uint8_t)c.tx_pin;
    q[16] = (uint8_t)c.rx_pin;
    q[17] = uart_state(line, &c);
    send(V_LINE, q, sizeof q);
}

/* GETTING without a payload answers for every line, with {U1 line} for that one. */
static int line_arg(const sbp_frame_t *f, int *first, int *last)
{
    if (f->len == 0) {
        *first = 0;
        *last = NLINES - 1;
        return 0;
    }
    if (f->len != 1 || f->payload[0] >= NLINES)
        return -1;
    *first = *last = f->payload[0];
    return 0;
}

static void set_line(const sbp_frame_t *f)
{
    const uint8_t *p = f->payload + 4;
    if (f->len != 4 + LINE_REC || p[0] >= NLINES) {
        sbpdev_ack(f, SBP_RESP_ERR_PAYLOAD);
        return;
    }
    int line = p[0];
    line_cfg_t c;
    netcfg_line(line, &c); /* keeps the saved rate: a new one goes through ports.c below */
    c.mode = p[1];
    c.dest = p[2];
    memcpy(c.ip, p + 3, 4);
    c.rport = sbp_get_u16(p + 7);
    c.lport = sbp_get_u16(p + 9);
    uint32_t baud = sbp_get_u32(p + 11);
    if (line == 1) { /* line 0's pins are fixed by the build */
        c.tx_pin = (int8_t)p[15];
        c.rx_pin = (int8_t)p[16];
    }
    sbp_chan_t ch;
    sbpdev_channel(&ch);
    /* 0 or the running rate keeps it. The line of the asking port keeps it too: that port's rate is
     * changed with ID_UART v0, and a host writing back a record it read earlier would send a stale rate. */
    bool rate = baud != 0 && baud != ports_baud(line) && sbpdev_chan_port(&ch) != line;
    if ((rate && !ports_baud_ok(baud)) || !netcfg_line_ok(line, &c)) {
        sbpdev_ack(f, SBP_RESP_ERR_PAYLOAD);
        return;
    }
    /* Answered after the change: the TX task sends it at once, before the network task (lower
     * priority) closes the socket of a changed line. The other port's new rate is saved at once. */
    bool ok = relay_line_set(line, &c);
    sbpdev_ack(f, ok ? SBP_RESP_OK : SBP_RESP_ERR_RUNTIME);
    if (ok && rate && !ports_set_baud(line, baud, false))
        ESP_LOGW(TAG, "line %d: rate %lu not applied", line, (unsigned long)baud);
    send_line(line);
}

/* ---- v4 line statistics ----------------------------------------------------------------------------- */

static void send_stats(int line)
{
    relay_stats_t r;
    relay_line_stats(line, &r);
    uint32_t ovf, drops;
    if (line == 0) {
        link_stats_t ls;
        link_get_stats(&ls);
        ovf = ls.rx_overflows;
        drops = ls.tx_dropped;
    } else {
        uline_stats_t us;
        uline_get_stats(&us);
        ovf = us.rx_overflows;
        drops = us.tx_drops;
    }
    uint8_t q[57];
    q[0] = (uint8_t)line;
    q[1] = r.state;
    memcpy(q + 2, r.peer_ip, 4);
    sbp_put_u16(q + 6, r.peer_port);
    q[8] = r.npeers;
    const uint32_t v[12] = { r.up_frames, r.up_bytes, r.up_packets, r.up_drops, r.down_packets, r.down_bytes,
                             r.down_frames, r.down_drops, r.local_frames, r.tcp_connects, ovf, drops };
    for (int i = 0; i < 12; i++)
        sbp_put_u32(q + 9 + 4 * i, v[i]);
    send(V_STATS, q, sizeof q);
}

/* ---- v5 access point clients ------------------------------------------------------------------------ */

static void send_clients(void)
{
    ap_client_t cl[10];
    int n = running_ap() ? wifiap_clients(cl, 10) : 0;
    if (n == 0) {
        const uint8_t q[2] = { 0, 0 };
        send(V_CLIENTS, q, sizeof q);
        return;
    }
    for (int i = 0; i < n; i++) {
        uint8_t q[13];
        q[0] = (uint8_t)i;
        q[1] = (uint8_t)n;
        memcpy(q + 2, cl[i].mac, 6);
        memcpy(q + 8, cl[i].ip, 4);
        q[12] = (uint8_t)cl[i].rssi;
        send(V_CLIENTS, q, sizeof q);
    }
}

/* ---- v7 port information ---------------------------------------------------------------------------- */

static void send_port(int port, int page)
{
    uint8_t q[PI_PAGE1_MAX > PI_PAGE0_LEN ? PI_PAGE1_MAX : PI_PAGE0_LEN];
    size_t n;
    if (page == 0) {
        sbp_chan_t ch;
        sbpdev_channel(&ch);
        uint8_t fl = 0;
        if (port == 0 || uline_running())
            fl |= PI_F_UART;
        if (relay_line_active(port))
            fl |= PI_F_BRIDGE;
        if (sbpdev_chan_port(&ch) == port)
            fl |= PI_F_ASKED;
        if (sbpdev_port_subscribed(port))
            fl |= PI_F_REPORTS;
        bool slip = port == 0 && link_proto() == LINK_PROTO_SLIP;
        if (slip)
            fl |= PI_F_SLIP;
        if (ports_provisional(port))
            fl |= PI_F_PROVISION;
#if CONFIG_WB_LINK_USB_SERIAL_JTAG
        if (port == 0)
            fl |= PI_F_USB;
#endif
        uint32_t ovf, drops;
        if (port == 0) {
            link_stats_t ls;
            link_get_stats(&ls);
            ovf = ls.rx_overflows;
            drops = ls.tx_dropped;
        } else {
            uline_stats_t us;
            uline_get_stats(&us);
            ovf = us.rx_overflows;
            drops = us.tx_drops;
        }
        n = portinfo_page0(port, fl, ports_baud(port), ports_saved_baud(port), ovf, drops, slip, q);
    } else if (page == 1) {
        n = portinfo_page1(port, q);
    } else {
        relay_peer_t pe[4];
        uint8_t mode, state;
        int k = relay_line_peers(port, &mode, &state, pe, 4);
        q[0] = (uint8_t)port;
        q[1] = 2;
        q[2] = mode;
        q[3] = state;
        q[4] = (uint8_t)k;
        for (int i = 0; i < k; i++) {
            memcpy(q + 5 + 8 * i, pe[i].ip, 4);
            sbp_put_u16(q + 9 + 8 * i, pe[i].port);
            sbp_put_u16(q + 11 + 8 * i, pe[i].age_ds);
        }
        n = 5 + 8u * (size_t)k;
    }
    send(V_PORTS, q, n);
}

/* GETTING v7: no payload = page 0 of both ports, {port} = its page 0, {port, page} = that page. */
static void get_ports(const sbp_frame_t *f)
{
    const uint8_t *p = f->payload;
    if (f->len > 2 || (f->len >= 1 && p[0] >= NLINES) || (f->len == 2 && p[1] > 2)) {
        sbpdev_ack(f, SBP_RESP_ERR_PAYLOAD);
        return;
    }
    if (f->len == 0) {
        for (int i = 0; i < NLINES; i++)
            send_port(i, 0);
        return;
    }
    send_port(p[0], f->len == 2 ? p[1] : 0);
}

/* ---- dispatcher ------------------------------------------------------------------------------------- */

void netctl_handle(const sbp_frame_t *f)
{
    uint8_t ver = sbp_ver(f->mode);
    if (ver > V_PORTS) {
        sbpdev_ack(f, SBP_RESP_ERR_VERSION);
        return;
    }
    if (sbp_type(f->mode) == SBP_T_GETTING) {
        int a, b;
        switch (ver) {
        case V_ROLE:    send_role(); break;
        case V_AP:      send_ap(); break;
        case V_IP:      send_ip(); break;
        case V_CLIENTS: send_clients(); break;
        case V_PORTS:   get_ports(f); break;
        case V_ADDR: {
            const uint8_t q[1] = { netcfg_addr() };
            send(V_ADDR, q, 1);
            break;
        }
        default: /* V_LINE, V_STATS */
            if (line_arg(f, &a, &b) != 0) {
                sbpdev_ack(f, SBP_RESP_ERR_PAYLOAD);
                break;
            }
            for (int i = a; i <= b; i++) {
                if (ver == V_LINE)
                    send_line(i);
                else
                    send_stats(i);
            }
        }
        return;
    }
    if (f->len < 4 || sbp_get_u32(f->payload) != SBP_KEY_CONFIRM) {
        sbpdev_ack(f, SBP_RESP_ERR_KEY);
        return;
    }
    switch (ver) {
    case V_ROLE: set_role(f); break;
    case V_AP:   set_ap(f); break;
    case V_IP:   set_ip(f); break;
    case V_LINE: set_line(f); break;
    case V_ADDR:
        if (f->len != 5 || f->payload[4] == 0 || f->payload[4] == 255) { /* a device's default / broadcast */
            sbpdev_ack(f, SBP_RESP_ERR_PAYLOAD);
            break;
        }
        sbpdev_ack(f, SBP_RESP_OK); /* from the old address, which the host matches */
        relay_set_addr(f->payload[4]);
        {
            const uint8_t q[1] = { netcfg_addr() };
            send(V_ADDR, q, 1);
        }
        break;
    default: /* statistics, clients and port information are read-only */
        sbpdev_ack(f, SBP_RESP_ERR_TYPE);
    }
}
