#include "relay.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "kframe.h"
#include "kpack.h"
#include "link.h"
#include "sbp.h"
#include "sbpdev.h"
#include "uline.h"

static const char *TAG = "relay";

#define FRAME_CAP   4096                 /* largest frame held whole, each direction (longer = noise) */
#define POOL_N      24                   /* 512-byte packets waiting for a socket, both lines: 12 KB */
#define RXBUF       1500                 /* one datagram / one TCP read */
#define MAXPEERS    4                    /* senders answered by a DEST_SENDERS line */
#define PEER_TTL_US (60LL * 1000 * 1000) /* a sender silent this long is forgotten */
#define IDLE_US     (200LL * 1000)       /* network side quiet this long: release a stuck candidate */
#define RETRY_US    (2LL * 1000 * 1000)  /* socket could not be opened: next attempt */
#define CONNECT_US  (3LL * 1000 * 1000)  /* TCP connect deadline */
#define TCP_SEND_US (5LL * 1000 * 1000)  /* TCP packet not accepted for this long: connection is dead */
#define MAX_PIECES  (FRAME_CAP / KP_MAX_PACKET + 1) /* packets one unit can make, incl. the one it pushes out */
#define EV_WIFI     BIT0                 /* station has an address / access point is running */
#define EV_RESET    BIT1                 /* a line changed or its TCP link broke */

typedef struct {
    uint8_t  line;
    uint8_t  idx;
    uint16_t len;
    uint32_t to_ip;    /* network order; to_port != 0: an answer of the module to this peer */
    uint16_t to_port;
} pkt_t;

typedef struct {
    line_cfg_t cfg;                      /* written under s_mux (manager task), then reset is raised */
    relay_stats_t st;
    bool ready;                          /* framer/packer buffers allocated */
    int sock;                            /* opened and closed only by the network task */
    int csock;                           /* TCP connect in progress (network task only) */
    volatile bool reset;
    int64_t retry_at, last_rx, conn_deadline;
    struct sockaddr_in peers[MAXPEERS];  /* DEST_SENDERS: who talked to us lately */
    int64_t seen[MAXPEERS];
    struct sockaddr_in last;             /* most recent sender, for the statistics */
    struct sockaddr_in rx_from;          /* sender of the data being framed (network task) */
    kf_t up;                             /* UART -> network: the line's UART RX task */
    kp_t pk;
    uint8_t resv[MAX_PIECES];            /* UART RX task: pool slots taken for the unit being packed */
    uint8_t nresv;
    kf_t dn;                             /* network -> UART: network task */
} rline_t;

static rline_t L[NLINES];
static EventGroupHandle_t s_ev;
static SemaphoreHandle_t s_mux;          /* sockets and configurations for the TX task, peer tables */
static uint8_t (*s_pool)[KP_MAX_PACKET];
static QueueHandle_t s_free, s_txq;
static uint8_t *s_rxbuf;
static esp_netif_t *s_netif;
static sbp_dec_t s_fdec;                 /* network task: module frames from senders a line ignores */

static uint32_t ip_net(const uint8_t *a)
{
    uint32_t v;
    memcpy(&v, a, 4); /* a.b.c.d in memory order = network order */
    return v;
}

static struct sockaddr_in sa(uint32_t ip, uint16_t port_host)
{
    return (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons(port_host), .sin_addr.s_addr = ip };
}

static uint16_t lport_of(const line_cfg_t *c)
{
    return c->lport ? c->lport : c->rport;
}

static bool line_on(int i)
{
    if (!L[i].ready || L[i].cfg.mode == LINE_OFF)
        return false;
    return i == 0 || uline_running();
}

bool relay_line_active(int line)
{
    return line >= 0 && line < NLINES && line_on(line);
}

bool relay_active(void)
{
    return line_on(0);
}

static bool uart_tx(int line, const uint8_t *d, size_t n)
{
    return line == 0 ? link_tx_relay(d, n) : uline_tx_relay(d, n);
}

static bool own_info(esp_netif_ip_info_t *ii)
{
    return s_netif && esp_netif_get_ip_info(s_netif, ii) == ESP_OK && ii->ip.addr != 0;
}

static uint32_t own_ip(void)
{
    esp_netif_ip_info_t ii;
    return own_info(&ii) ? ii.ip.addr : 0;
}

static uint32_t bcast_ip(void)
{
    esp_netif_ip_info_t ii;
    return own_info(&ii) ? (ii.ip.addr & ii.netmask.addr) | ~ii.netmask.addr : 0;
}

/* ---- configuration -------------------------------------------------------------------------- */

/* The module's own address while any line bridges (a bridged device keeps its address, usually 0),
 * the boot address otherwise; the host port carries only SBP while line 0 bridges. */
static void apply_route(void)
{
    bool any = line_on(0) || line_on(1);
    sbpdev_set_route(any ? netcfg_addr() : sbpdev_default_route());
    if (line_on(0))
        link_set_proto(LINK_PROTO_SBP);
}

static void log_line(int i)
{
    const line_cfg_t *c = &L[i].cfg;
    ESP_LOGI(TAG, "line %d: mode %u dest %u peer %u.%u.%u.%u:%u lport %u %s", i, c->mode, c->dest, c->ip[0],
             c->ip[1], c->ip[2], c->ip[3], c->rport, lport_of(c), line_on(i) ? "on" : "off");
}

static bool net_differs(const line_cfg_t *a, const line_cfg_t *b)
{
    return a->mode != b->mode || a->dest != b->dest || memcmp(a->ip, b->ip, 4) || a->rport != b->rport ||
           a->lport != b->lport;
}

static void take_cfg(int line, const line_cfg_t *c)
{
    line_cfg_t old = L[line].cfg;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    L[line].cfg = *c;
    xSemaphoreGive(s_mux);
    if (line == 1 && c->baud != old.baud)
        uline_set_baud(c->baud); /* pins take effect after a reboot: a running UART is never re-pinned */
    if (net_differs(&old, c)) {
        L[line].reset = true;
        xEventGroupSetBits(s_ev, EV_RESET);
    }
    apply_route();
    log_line(line);
}

bool relay_line_set(int line, const line_cfg_t *c)
{
    if (line < 0 || line >= NLINES || !netcfg_set_line(line, c))
        return false;
    take_cfg(line, c);
    return true;
}

bool relay_set_addr(uint8_t addr)
{
    if (!netcfg_set_addr(addr))
        return false;
    apply_route();
    return true;
}

void relay_get_cfg(relay_cfg_t *c)
{
    const line_cfg_t *l = &L[0].cfg;
    c->mode = l->mode;
    c->addr = netcfg_addr();
    if (l->dest == DEST_FIXED || l->mode == LINE_TCP)
        memcpy(c->ip, l->ip, 4);
    else
        memset(c->ip, l->dest == DEST_BROADCAST ? 0xFF : 0, 4);
    c->rport = l->rport;
    c->lport = l->lport;
}

static void v5_to_line(const relay_cfg_t *c, line_cfg_t *l)
{
    *l = L[0].cfg;
    l->mode = c->mode;
    memcpy(l->ip, c->ip, 4);
    l->rport = c->rport;
    l->lport = c->lport;
    uint32_t ip = ip_net(c->ip);
    l->dest = ip == 0 ? DEST_SENDERS : ip == 0xFFFFFFFFu ? DEST_BROADCAST : DEST_FIXED;
}

bool relay_cfg_ok(const relay_cfg_t *c)
{
    if (c->mode > RELAY_TCP || (c->mode != RELAY_OFF && c->addr == 0))
        return false; /* bridging on address 0 would swallow the frames of a device on its default address */
    line_cfg_t l;
    v5_to_line(c, &l);
    return netcfg_line_ok(0, &l);
}

bool relay_set_cfg(const relay_cfg_t *c)
{
    if (!relay_cfg_ok(c))
        return false;
    line_cfg_t l;
    v5_to_line(c, &l);
    if (!netcfg_set_line(0, &l))
        return false;
    if (c->mode != RELAY_OFF) /* switching off leaves the address alone: it also serves line 1 */
        netcfg_set_addr(c->addr);
    take_cfg(0, &l);
    return true;
}

void relay_wifi(bool up)
{
    if (up)
        xEventGroupSetBits(s_ev, EV_WIFI);
    else
        xEventGroupClearBits(s_ev, EV_WIFI);
}

void relay_set_netif(esp_netif_t *netif)
{
    s_netif = netif;
}

/* ---- UART -> network (the line's UART RX task) -------------------------------------------------- */

static bool take_slot(uint8_t *idx)
{
    return xQueueReceive(s_free, idx, 0) == pdTRUE;
}

static void put_pkt(int line, uint8_t idx, const uint8_t *d, size_t n, uint32_t to_ip, uint16_t to_port)
{
    memcpy(s_pool[idx], d, n);
    pkt_t it = { .line = (uint8_t)line, .idx = idx, .len = (uint16_t)n, .to_ip = to_ip, .to_port = to_port };
    xQueueSend(s_txq, &it, 0); /* cannot fail: as many slots as packets */
}

static void up_send(void *ctx, const uint8_t *pkt, size_t len)
{
    int line = (int)(intptr_t)ctx;
    rline_t *l = &L[line];
    uint8_t idx;
    if (l->nresv) {
        idx = l->resv[--l->nresv];
    } else if (!take_slot(&idx)) {
        l->st.up_drops++; /* the socket side is behind: drop this packet (whole units only) */
        return;
    }
    put_pkt(line, idx, pkt, len, 0, 0);
}

/* The packets a unit makes - its pieces when it is longer than a packet, and the collected packet it
 * pushes out - get their pool slots all at once or the unit is dropped whole: a peer never receives
 * some pieces of a frame without the others. */
static bool reserve(rline_t *l, size_t n)
{
    size_t max = l->pk.max, len = l->pk.len, need = 0;
    if (n > max - len)
        need = (len ? 1u : 0u) + (n > max ? (n + max - 1) / max : 0u);
    while (l->nresv < need) {
        if (!take_slot(&l->resv[l->nresv])) {
            while (l->nresv)
                xQueueSend(s_free, &l->resv[--l->nresv], 0);
            return false;
        }
        l->nresv++;
    }
    return true;
}

static bool for_module(const uint8_t *d, unsigned flags)
{
    if ((flags & KF_P_MASK) != KF_P_KP1 || d[2] != sbpdev_route())
        return false;
    uint8_t t = d[3] & 3u;
    return t == SBP_T_SETTING || t == SBP_T_GETTING;
}

static void to_module(rline_t *l, const uint8_t *d, const sbp_chan_t *ch)
{
    sbp_frame_t f = { d[2], d[3], d[4], d[5], d + 6, d[6 + d[5]], d[7 + d[5]] };
    l->st.local_frames++;
    sbpdev_on_frame_ch(&f, ch);
}

static void up_unit(void *ctx, kf_kind_t kind, const uint8_t *d, size_t n, unsigned flags)
{
    int line = (int)(intptr_t)ctx;
    rline_t *l = &L[line];
    if (kind == KF_FRAME && for_module(d, flags)) {
        const sbp_chan_t ch = { .kind = line == 0 ? CH_LINK : CH_LINE1, .line = (uint8_t)line };
        to_module(l, d, &ch); /* never relayed */
        return;
    }
    if (!line_on(line))
        return; /* a line that does not bridge is only listened to for the module's own frames */
    if (!reserve(l, n)) {
        l->st.up_drops++;
        return;
    }
    if (kind == KF_FRAME)
        l->st.up_frames++;
    l->st.up_bytes += (uint32_t)n;
    kp_unit(&l->pk, d, n, up_send, ctx);
    while (l->nresv) /* none should be left: the count above is exactly what kp_unit sends */
        xQueueSend(s_free, &l->resv[--l->nresv], 0);
}

void relay_line_bytes(int line, const uint8_t *d, size_t n)
{
    if (L[line].ready)
        kf_feed(&L[line].up, d, n, up_unit, (void *)(intptr_t)line);
}

void relay_line_idle(int line, bool long_idle)
{
    if (!L[line].ready)
        return;
    kf_flush(&L[line].up, long_idle, up_unit, (void *)(intptr_t)line);
    kp_flush(&L[line].pk, up_send, (void *)(intptr_t)line); /* whole units only: an unfinished frame waits */
}

void relay_uart_bytes(const uint8_t *d, size_t n)
{
    relay_line_bytes(0, d, n);
}

void relay_uart_idle(bool long_idle)
{
    relay_line_idle(0, long_idle);
}

void relay_send_to(int line, uint32_t ip, uint16_t port, const uint8_t *d, size_t n)
{
    uint8_t idx;
    if (n > KP_MAX_PACKET || !take_slot(&idx)) {
        L[line].st.up_drops++;
        return;
    }
    put_pkt(line, idx, d, n, ip, port);
}

/* ---- network TX task ---------------------------------------------------------------------------------- */

static void net_tx_task(void *arg)
{
    pkt_t it;
    for (;;) {
        if (xQueueReceive(s_txq, &it, portMAX_DELAY) != pdTRUE)
            continue;
        rline_t *l = &L[it.line];
        struct sockaddr_in to[MAXPEERS];
        int nto = 0;
        uint32_t self = own_ip();
        xSemaphoreTake(s_mux, portMAX_DELAY);
        int sock = l->sock;
        bool tcp = l->cfg.mode == LINE_TCP;
        if (it.to_port) {
            to[nto++] = (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = it.to_port, .sin_addr.s_addr = it.to_ip };
        } else if (!tcp) {
            if (l->cfg.dest == DEST_FIXED) {
                if (ip_net(l->cfg.ip) != self) /* never to ourselves: loopback would echo it back */
                    to[nto++] = sa(ip_net(l->cfg.ip), l->cfg.rport);
            } else if (l->cfg.dest == DEST_BROADCAST) {
                uint32_t b = bcast_ip();
                if (b)
                    to[nto++] = sa(b, l->cfg.rport);
            } else {
                int64_t now = esp_timer_get_time();
                for (int p = 0; p < MAXPEERS; p++)
                    if (l->seen[p] && now - l->seen[p] < PEER_TTL_US)
                        to[nto++] = l->peers[p];
            }
        }
        xSemaphoreGive(s_mux);

        bool ok = false;
        if (sock >= 0 && tcp) {
            size_t off = 0;
            int64_t give_up = esp_timer_get_time() + TCP_SEND_US;
            while (off < it.len) { /* blocking with SO_SNDTIMEO: the TCP window is the back-pressure */
                int w = send(sock, s_pool[it.idx] + off, it.len - off, 0);
                if (w > 0) {
                    off += (size_t)w;
                    continue;
                }
                /* A second without progress is a retransmission (initial RTO 1.5 s), not a dead link. */
                if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && esp_timer_get_time() < give_up)
                    continue;
                break;
            }
            ok = off == it.len;
            if (!ok) {
                l->reset = true; /* connection is gone: reconnect */
                xEventGroupSetBits(s_ev, EV_RESET);
            }
        } else if (sock >= 0) {
            for (int k = 0; k < nto; k++) /* UDP does not retry: a full Wi-Fi queue is a lost packet */
                if (sendto(sock, s_pool[it.idx], it.len, 0, (struct sockaddr *)&to[k], sizeof to[k]) == it.len)
                    ok = true;
        }
        if (ok)
            l->st.up_packets++;
        else
            l->st.up_drops++;
        xQueueSend(s_free, &it.idx, 0);
    }
}

/* ---- network task: sockets of both lines, network -> UART ---------------------------------------------- */

static void dn_unit(void *ctx, kf_kind_t kind, const uint8_t *d, size_t n, unsigned flags)
{
    int line = (int)(intptr_t)ctx;
    rline_t *l = &L[line];
    if (kind == KF_FRAME && for_module(d, flags)) { /* configured over the network: answer the sender */
        const sbp_chan_t ch = { .kind = CH_NET, .line = (uint8_t)line, .ip = l->rx_from.sin_addr.s_addr,
                                .port = l->rx_from.sin_port };
        to_module(l, d, &ch);
        return;
    }
    if (!uart_tx(line, d, n)) {
        l->st.down_drops++; /* UART cannot keep up: drop the whole unit, never a piece of it */
        return;
    }
    if (kind == KF_FRAME)
        l->st.down_frames++;
}

typedef struct {
    int line;
    struct sockaddr_in from;
} foreign_t;

static void foreign_frame(void *ctx, const sbp_frame_t *f)
{
    const foreign_t *fo = ctx;
    uint8_t t = sbp_type(f->mode);
    if (f->route != sbpdev_route() || (t != SBP_T_SETTING && t != SBP_T_GETTING))
        return;
    const sbp_chan_t ch = { .kind = CH_NET, .line = (uint8_t)fo->line, .ip = fo->from.sin_addr.s_addr,
                            .port = fo->from.sin_port };
    L[fo->line].st.local_frames++;
    sbpdev_on_frame_ch(f, &ch);
}

/* A datagram from somebody the line does not relay for (not its fixed peer): only whole frames
 * addressed to the module are taken from it, so the module stays reachable on every open port. */
static void foreign_datagram(int line, const struct sockaddr_in *from, const uint8_t *d, size_t n)
{
    foreign_t fo = { .line = line, .from = *from };
    sbp_dec_init(&s_fdec);
    sbp_dec_feed(&s_fdec, d, n, foreign_frame, &fo);
}

static void sock_close(rline_t *l)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    int s = l->sock;
    l->sock = -1;
    xSemaphoreGive(s_mux);
    if (s >= 0) {
        shutdown(s, SHUT_RDWR);
        close(s);
    }
    if (l->csock >= 0) {
        close(l->csock);
        l->csock = -1;
    }
}

static int open_udp(const line_cfg_t *c)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0)
        return -1;
    struct sockaddr_in a = sa(htonl(INADDR_ANY), lport_of(c));
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
        close(s);
        return -1;
    }
    if (c->dest == DEST_BROADCAST) {
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    }
    return s;
}

/* The connect runs in the background of the select loop, so the other line keeps being served. */
static int tcp_begin(const line_cfg_t *c, bool *done)
{
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0)
        return -1;
    struct sockaddr_in a = sa(ip_net(c->ip), c->rport);
    fcntl(s, F_SETFL, O_NONBLOCK);
    int r = connect(s, (struct sockaddr *)&a, sizeof a);
    if (r != 0 && errno != EINPROGRESS) {
        close(s);
        return -1;
    }
    *done = r == 0;
    return s;
}

static void tcp_ready(int i, int s)
{
    rline_t *l = &L[i];
    fcntl(s, F_SETFL, 0); /* blocking sends with SO_SNDTIMEO: the TCP window is the back-pressure */
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); /* frames are small: no Nagle delay */
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    struct timeval st = { .tv_sec = 1 };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &st, sizeof st);
    xSemaphoreTake(s_mux, portMAX_DELAY);
    l->sock = s;
    xSemaphoreGive(s_mux);
    l->st.tcp_connects++;
    l->rx_from = sa(ip_net(l->cfg.ip), l->cfg.rport);
    ESP_LOGI(TAG, "line %d: TCP connected", i);
}

static void tcp_check(int i, bool writable, int64_t now)
{
    rline_t *l = &L[i];
    int err = 0;
    socklen_t el = sizeof err;
    if (writable && getsockopt(l->csock, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0) {
        int s = l->csock;
        l->csock = -1;
        tcp_ready(i, s);
    } else if (writable || now > l->conn_deadline) {
        close(l->csock); /* refused, unreachable or no answer in time */
        l->csock = -1;
        l->retry_at = now + RETRY_US;
    }
}

static void note_sender(rline_t *l, const struct sockaddr_in *from, int64_t now)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    int slot = 0;
    for (int p = 0; p < MAXPEERS; p++) {
        if (l->seen[p] && l->peers[p].sin_addr.s_addr == from->sin_addr.s_addr && l->peers[p].sin_port == from->sin_port) {
            slot = p;
            break;
        }
        if (l->seen[p] < l->seen[slot])
            slot = p; /* otherwise the stalest entry (0 = free) */
    }
    l->peers[slot] = *from;
    l->seen[slot] = now;
    l->last = *from;
    xSemaphoreGive(s_mux);
}

static bool has_dest(const rline_t *l, int64_t now)
{
    if (l->cfg.mode == LINE_TCP)
        return true; /* connected */
    if (l->cfg.dest == DEST_FIXED)
        return ip_net(l->cfg.ip) != own_ip();
    if (l->cfg.dest == DEST_BROADCAST)
        return bcast_ip() != 0;
    for (int p = 0; p < MAXPEERS; p++)
        if (l->seen[p] && now - l->seen[p] < PEER_TTL_US)
            return true;
    return false;
}

static void on_readable(int i, int64_t now)
{
    rline_t *l = &L[i];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    bool udp = l->cfg.mode == LINE_UDP;
    int n = udp ? recvfrom(l->sock, s_rxbuf, RXBUF, MSG_DONTWAIT, (struct sockaddr *)&from, &fl)
                : recv(l->sock, s_rxbuf, RXBUF, MSG_DONTWAIT);
    if (n > 0) {
        if (udp) {
            if (from.sin_addr.s_addr == own_ip())
                return; /* our own broadcast coming back: never into the UART again */
            if (l->cfg.dest == DEST_FIXED && from.sin_addr.s_addr != ip_net(l->cfg.ip)) {
                foreign_datagram(i, &from, s_rxbuf, (size_t)n); /* not our peer: nothing is relayed */
                return;
            }
            l->rx_from = from;
            note_sender(l, &from, now);
        }
        l->st.down_packets++;
        l->st.down_bytes += (uint32_t)n;
        l->last_rx = now;
        kf_feed(&l->dn, s_rxbuf, (size_t)n, dn_unit, (void *)(intptr_t)i);
        kf_flush(&l->dn, false, dn_unit, (void *)(intptr_t)i); /* end of a datagram/read: raw bytes out */
    } else if ((n == 0 && !udp) || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        sock_close(l); /* peer closed or socket failed: reopened later */
        l->retry_at = now + RETRY_US;
    }
}

static void net_task(void *arg)
{
    for (;;) {
        EventBits_t ev = xEventGroupClearBits(s_ev, EV_RESET);
        bool up = (ev & EV_WIFI) != 0;
        int64_t now = esp_timer_get_time();
        fd_set rf, wf;
        FD_ZERO(&rf);
        FD_ZERO(&wf);
        int maxfd = -1;
        for (int i = 0; i < NLINES; i++) {
            rline_t *l = &L[i];
            if (l->reset) {
                l->reset = false;
                sock_close(l);
                if (l->ready)
                    kf_flush(&l->dn, true, dn_unit, (void *)(intptr_t)i);
                l->retry_at = 0;
            }
            if (!line_on(i) || !up) {
                if (l->sock >= 0 || l->csock >= 0)
                    sock_close(l);
                l->st.state = line_on(i) ? RS_NO_WIFI : RS_OFF;
                continue;
            }
            if (l->sock < 0 && l->csock < 0 && now >= l->retry_at) {
                if (l->cfg.mode == LINE_TCP) {
                    bool done = false;
                    int s = tcp_begin(&l->cfg, &done);
                    if (s >= 0 && done) {
                        tcp_ready(i, s);
                    } else if (s >= 0) {
                        l->csock = s;
                        l->conn_deadline = now + CONNECT_US;
                    } else {
                        l->retry_at = now + RETRY_US;
                    }
                } else {
                    int s = open_udp(&l->cfg);
                    if (s >= 0) {
                        xSemaphoreTake(s_mux, portMAX_DELAY);
                        l->sock = s;
                        xSemaphoreGive(s_mux);
                        ESP_LOGI(TAG, "line %d: UDP port %u open", i, lport_of(&l->cfg));
                    } else {
                        l->retry_at = now + RETRY_US; /* port taken, no memory */
                    }
                }
            }
            if (l->csock >= 0) {
                l->st.state = RS_NO_PEER;
                FD_SET(l->csock, &wf);
                if (l->csock > maxfd)
                    maxfd = l->csock;
                continue;
            }
            if (l->sock < 0) {
                l->st.state = RS_NO_PEER;
                continue;
            }
            l->st.state = has_dest(l, now) ? RS_READY : RS_NO_PEER;
            FD_SET(l->sock, &rf);
            if (l->sock > maxfd)
                maxfd = l->sock;
        }
        if (maxfd < 0) {
            /* Nothing to listen to: sleep until something changes. EV_WIFI stays set while connected,
             * so waiting on it then would return at once and spin (0.7/0.8 did, starving the manager). */
            xEventGroupWaitBits(s_ev, up ? EV_RESET : EV_WIFI | EV_RESET, pdFALSE, pdFALSE, pdMS_TO_TICKS(500));
            continue;
        }
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 }; /* also bounds the reaction to a change */
        int r = select(maxfd + 1, &rf, &wf, NULL, &tv);
        if (r < 0)
            vTaskDelay(1); /* select fails at once when out of memory: do not spin */
        now = esp_timer_get_time();
        for (int i = 0; i < NLINES; i++) {
            rline_t *l = &L[i];
            if (l->csock >= 0) {
                tcp_check(i, r > 0 && FD_ISSET(l->csock, &wf), now);
                continue;
            }
            if (l->sock < 0)
                continue;
            if (r > 0 && FD_ISSET(l->sock, &rf))
                on_readable(i, now);
            else if (kf_pending(&l->dn) && now - l->last_rx > IDLE_US)
                kf_flush(&l->dn, true, dn_unit, (void *)(intptr_t)i); /* release a stuck candidate */
        }
    }
}

/* ---- life cycle ------------------------------------------------------------------------------------- */

static bool alloc_line(int i)
{
    uint8_t *fb = malloc(4 * FRAME_CAP);
    if (!fb)
        return false;
    kf_init(&L[i].up, fb, fb + FRAME_CAP, FRAME_CAP);
    kf_init(&L[i].dn, fb + 2 * FRAME_CAP, fb + 3 * FRAME_CAP, FRAME_CAP);
    kp_init(&L[i].pk, KP_MAX_PACKET);
    L[i].ready = true; /* last: the UART RX task starts feeding once it sees this */
    return true;
}

void relay_init(void)
{
    s_ev = xEventGroupCreate();
    s_mux = xSemaphoreCreateMutex();
    s_pool = malloc(POOL_N * KP_MAX_PACKET);
    s_rxbuf = malloc(RXBUF);
    s_free = xQueueCreate(POOL_N, sizeof(uint8_t));
    s_txq = xQueueCreate(POOL_N, sizeof(pkt_t));
    configASSERT(s_ev && s_mux && s_pool && s_rxbuf && s_free && s_txq);
    for (uint8_t i = 0; i < POOL_N; i++)
        xQueueSend(s_free, &i, 0);
    for (int i = 0; i < NLINES; i++) {
        L[i].sock = L[i].csock = -1;
        netcfg_line(i, &L[i].cfg);
    }
    bool ok = alloc_line(0);
    configASSERT(ok);
    (void)ok;
}

void relay_start(void)
{
    if (uline_running() && !alloc_line(1)) /* line 1 buffers only when its UART runs */
        ESP_LOGE(TAG, "no memory for line 1");
    /* Before the link starts, so no frame is ever judged against a wrong address. A one-shot address
     * kept across an update or role reboot wins; otherwise the bridging address or the boot one. */
    if ((line_on(0) || line_on(1)) && !sbpdev_route_resumed())
        sbpdev_set_route(netcfg_addr());
    if (line_on(0))
        link_set_proto(LINK_PROTO_SBP);
    for (int i = 0; i < NLINES; i++)
        log_line(i);
    xTaskCreate(net_tx_task, "relay_tx", 3072, NULL, 10, NULL);
    xTaskCreate(net_task, "relay_net", 4096, NULL, 9, NULL);
}

void relay_line_stats(int line, relay_stats_t *s)
{
    rline_t *l = &L[line];
    int64_t now = esp_timer_get_time();
    uint32_t b = bcast_ip();
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *s = l->st;
    s->npeers = 0;
    for (int p = 0; p < MAXPEERS; p++)
        if (l->seen[p] && now - l->seen[p] < PEER_TTL_US)
            s->npeers++;
    struct sockaddr_in peer = { 0 };
    if (l->cfg.mode == LINE_TCP || l->cfg.dest == DEST_FIXED)
        peer = sa(ip_net(l->cfg.ip), l->cfg.rport);
    else if (l->cfg.dest == DEST_BROADCAST)
        peer = sa(b, l->cfg.rport);
    else if (s->npeers)
        peer = l->last;
    xSemaphoreGive(s_mux);
    memcpy(s->peer_ip, &peer.sin_addr.s_addr, 4);
    s->peer_port = ntohs(peer.sin_port);
}

void relay_get_stats(relay_stats_t *s)
{
    relay_line_stats(0, s);
}
