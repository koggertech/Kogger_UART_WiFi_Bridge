#include "portinfo.h"

#include <string.h>

#include "kframe.h"
#include "sbp.h"

#define RECENT_DS   100u   /* "connected" looks at the last 10 s */
#define ID_VERSION  0x20

typedef struct {
    uint32_t up, down, up_t;
} pi_proto_t;

typedef struct {
    uint8_t  kind;         /* 0 free, 1 SBP device, 2 MAVLink system */
    uint8_t  addr;         /* SBP route / MAVLink sysid */
    uint8_t  a, b, c;      /* board, fw major, fw minor / compid, MAV_TYPE, MAV_AUTOPILOT */
    uint8_t  flags;        /* b0 SBP version known, b1 heartbeat seen */
    uint32_t t;
    uint32_t serial;       /* SBP serial, or MAVLink frames seen */
} pi_dev_t;

typedef struct {
    uint32_t rx_bytes, tx_bytes, rx_t, tx_t;
    pi_proto_t p[PI_NPROTO];
    uint32_t req_up, content_up, req_dn, content_dn, mod_rx, mod_tx;
    uint32_t req_up_t, content_up_t;
    pi_dev_t dev[PI_MAXDEV];
} pi_port_t;

static pi_port_t P[PI_PORTS];
static uint32_t (*s_now)(void);
static void (*s_lock)(void), (*s_unlock)(void);

static uint32_t now(void)
{
    uint32_t t = s_now ? s_now() : 0;
    return t ? t : 1; /* 0 means "never" */
}

static void lock(void)
{
    if (s_lock)
        s_lock();
}

static void unlock(void)
{
    if (s_unlock)
        s_unlock();
}

static bool ok_port(int port)
{
    return port >= 0 && port < PI_PORTS;
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

void portinfo_init(uint32_t (*now_ds)(void), void (*lk)(void), void (*ulk)(void))
{
    memset(P, 0, sizeof P);
    s_now = now_ds;
    s_lock = lk;
    s_unlock = ulk;
}

int portinfo_proto(int kf_kind, unsigned kf_flags)
{
    if (kf_kind != KF_FRAME)
        return PI_RAW;
    switch (kf_flags & KF_P_MASK) {
    case KF_P_KP1:  return PI_KP1;
    case KF_P_KP2:  return PI_KP2;
    case KF_P_UBX:  return PI_UBX;
    case KF_P_MAV1: return PI_MAV1;
    case KF_P_MAV2: return PI_MAV2;
    default:        return PI_RAW;
    }
}

uint16_t portinfo_age(uint32_t t)
{
    if (!t)
        return 0xFFFF;
    uint32_t a = now() - t;
    return a >= 0xFFFF ? 0xFFFF : (uint16_t)a;
}

static bool recent(uint32_t t, uint32_t n)
{
    return t && n - t <= RECENT_DS;
}

void portinfo_rx_bytes(int port, size_t n)
{
    if (!ok_port(port) || !n)
        return;
    P[port].rx_bytes += (uint32_t)n;
    P[port].rx_t = now();
}

void portinfo_tx_bytes(int port, size_t n)
{
    if (!ok_port(port) || !n)
        return;
    P[port].tx_bytes += (uint32_t)n;
    P[port].tx_t = now();
}

/* The entry of a device, a free one, or the stalest one. Called under the lock. */
static pi_dev_t *dev_slot(pi_port_t *pp, uint8_t kind, uint8_t addr, uint8_t sub, bool by_sub)
{
    pi_dev_t *free_e = NULL, *old = &pp->dev[0];
    for (int i = 0; i < PI_MAXDEV; i++) {
        pi_dev_t *e = &pp->dev[i];
        if (e->kind == kind && e->addr == addr && (!by_sub || e->a == sub))
            return e;
        if (!e->kind && !free_e)
            free_e = e;
        if (e->t < old->t)
            old = e;
    }
    pi_dev_t *e = free_e ? free_e : old;
    memset(e, 0, sizeof *e);
    e->kind = kind;
    e->addr = addr;
    return e;
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* A CONTENT frame from the port: a device at its route; ID_VERSION tells board, firmware, serial. */
static void sbp_device(pi_port_t *pp, const uint8_t *d, size_t n, uint32_t t)
{
    uint8_t mode = d[3], id = d[4], len = d[5];
    const uint8_t *pl = d + 6;
    lock();
    pi_dev_t *e = dev_slot(pp, 1, d[2], 0, false);
    e->t = t;
    if (id == ID_VERSION && !sbp_resp(mode) && (size_t)len + 8 <= n) {
        uint8_t ver = sbp_ver(mode);
        if (ver == 0 && len >= 18) {
            e->a = pl[1];
            e->serial = get32(pl + 14);
        } else if (ver == 2 && len >= 9) {
            e->a = pl[2];
            e->b = pl[8];
            e->c = pl[7];
            e->flags |= 1u;
        }
    }
    unlock();
}

/* MAVLink: the system and component of any frame; type and autopilot from HEARTBEAT (id 0). */
static void mav_system(pi_port_t *pp, bool v2, const uint8_t *d, size_t n, uint32_t t)
{
    size_t hdr = v2 ? 10 : 6;
    if (n < hdr + 2)
        return;
    uint8_t len = d[1], sys = v2 ? d[5] : d[3], comp = v2 ? d[6] : d[4];
    uint32_t msgid = v2 ? (uint32_t)d[7] | (uint32_t)d[8] << 8 | (uint32_t)d[9] << 16 : d[5];
    lock();
    pi_dev_t *e = dev_slot(pp, 2, sys, comp, true);
    e->a = comp;
    e->t = t;
    e->serial++;
    if (msgid == 0) {
        /* custom_mode U4, type, autopilot, base_mode, system_status, version; MAVLink 2 trims trailing
         * zeros from the payload, so a missing byte reads as 0 */
        const uint8_t *pl = d + hdr;
        e->b = len > 4 ? pl[4] : 0;
        e->c = len > 5 ? pl[5] : 0;
        e->flags |= 2u;
    }
    unlock();
}

void portinfo_up(int port, int proto, const uint8_t *d, size_t n)
{
    if (!ok_port(port) || proto < 0 || proto >= PI_NPROTO)
        return;
    pi_port_t *pp = &P[port];
    uint32_t t = now();
    pp->p[proto].up++;
    pp->p[proto].up_t = t;
    if (proto == PI_KP1 && n >= 8) {
        uint8_t type = sbp_type(d[3]);
        if (type == SBP_T_SETTING || type == SBP_T_GETTING) {
            pp->req_up++;
            pp->req_up_t = t;
        } else if (type == SBP_T_CONTENT) {
            pp->content_up++;
            pp->content_up_t = t;
            sbp_device(pp, d, n, t);
        }
    } else if (proto == PI_MAV1 || proto == PI_MAV2) {
        mav_system(pp, proto == PI_MAV2, d, n, t);
    }
}

void portinfo_down(int port, int proto, const uint8_t *d, size_t n)
{
    if (!ok_port(port) || proto < 0 || proto >= PI_NPROTO)
        return;
    pi_port_t *pp = &P[port];
    pp->p[proto].down++;
    if (proto == PI_KP1 && n >= 8) {
        uint8_t type = sbp_type(d[3]);
        if (type == SBP_T_SETTING || type == SBP_T_GETTING)
            pp->req_dn++;
        else if (type == SBP_T_CONTENT)
            pp->content_dn++;
    }
}

void portinfo_module_rx(int port)
{
    if (ok_port(port))
        P[port].mod_rx++;
}

void portinfo_module_tx(int port)
{
    if (ok_port(port))
        P[port].mod_tx++;
}

static uint8_t connected(const pi_port_t *pp, bool slip, uint32_t n)
{
    if (slip && recent(pp->rx_t, n))
        return PI_C_SLIP;
    bool host = recent(pp->req_up_t, n);
    bool dev = recent(pp->content_up_t, n) || recent(pp->p[PI_KP2].up_t, n);
    if (host && dev)
        return PI_C_HOST_DEVICE;
    if (host)
        return PI_C_HOST;
    if (dev)
        return PI_C_DEVICE;
    if (recent(pp->p[PI_MAV1].up_t, n) || recent(pp->p[PI_MAV2].up_t, n))
        return PI_C_MAVLINK;
    if (recent(pp->p[PI_UBX].up_t, n))
        return PI_C_UBLOX;
    if (recent(pp->p[PI_RAW].up_t, n) || recent(pp->rx_t, n))
        return PI_C_UNREADABLE;
    return PI_C_NONE;
}

size_t portinfo_page0(int port, uint8_t flags, uint32_t baud, uint32_t saved, uint32_t rx_overflows,
                      uint32_t tx_drops, bool slip, uint8_t *out)
{
    if (!ok_port(port))
        return 0;
    lock();
    pi_port_t s = P[port];
    unlock();
    uint32_t n = now();
    memset(out, 0, PI_PAGE0_LEN);
    out[0] = (uint8_t)port;
    out[1] = 0;
    out[2] = flags;
    put32(out + 3, baud);
    put32(out + 7, saved);
    out[11] = connected(&s, slip, n);
    put32(out + 12, s.rx_bytes);
    put32(out + 16, s.tx_bytes);
    put16(out + 20, portinfo_age(s.rx_t));
    put16(out + 22, portinfo_age(s.tx_t));
    for (int i = 0; i < PI_NPROTO; i++) {
        uint8_t *q = out + 24 + 10 * i;
        put32(q, s.p[i].up);
        put32(q + 4, s.p[i].down);
        put16(q + 8, portinfo_age(s.p[i].up_t));
    }
    const uint32_t v[8] = { s.req_up, s.content_up, s.req_dn, s.content_dn, s.mod_rx, s.mod_tx, rx_overflows, tx_drops };
    for (int i = 0; i < 8; i++)
        put32(out + 84 + 4 * i, v[i]);
    return PI_PAGE0_LEN;
}

size_t portinfo_page1(int port, uint8_t *out)
{
    if (!ok_port(port))
        return 0;
    pi_dev_t d[PI_MAXDEV];
    lock();
    memcpy(d, P[port].dev, sizeof d);
    unlock();
    out[0] = (uint8_t)port;
    out[1] = 1;
    uint8_t cnt = 0;
    for (;;) { /* most recent first */
        int best = -1;
        for (int i = 0; i < PI_MAXDEV; i++)
            if (d[i].kind && (best < 0 || d[i].t > d[best].t))
                best = i;
        if (best < 0)
            break;
        uint8_t *q = out + 3 + 12 * cnt++;
        const pi_dev_t *e = &d[best];
        q[0] = e->kind;
        q[1] = e->addr;
        q[2] = e->a;
        q[3] = e->b;
        q[4] = e->c;
        q[5] = e->flags;
        put16(q + 6, portinfo_age(e->t));
        put32(q + 8, e->serial);
        d[best].kind = 0;
    }
    out[2] = cnt;
    return 3 + 12u * cnt;
}
