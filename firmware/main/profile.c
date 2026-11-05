/*
 * profile.c - the module profile blob (profile.h).
 */
#include "profile.h"

#include <string.h>

typedef struct {
    const uint8_t *b;
    size_t         n, at;
    bool           bad;
} rd_t;

static uint8_t u8(rd_t *r)
{
    if (r->at + 1 > r->n) {
        r->bad = true;
        return 0;
    }
    return r->b[r->at++];
}

static uint16_t u16(rd_t *r)
{
    uint16_t v = u8(r);
    return (uint16_t)(v | (u8(r) << 8));
}

static uint32_t u32(rd_t *r)
{
    uint32_t v = u16(r);
    return v | ((uint32_t)u16(r) << 16);
}

static void bytes(rd_t *r, uint8_t *out, size_t k)
{
    if (r->at + k > r->n) {
        r->bad = true;
        return;
    }
    memcpy(out, r->b + r->at, k);
    r->at += k;
}

/* A length-prefixed string of min..max bytes without a NUL inside. */
static bool str(rd_t *r, char *out, size_t min, size_t max)
{
    size_t k = u8(r);
    if (r->bad || k < min || k > max || r->at + k > r->n || memchr(r->b + r->at, 0, k)) {
        r->bad = true;
        return false;
    }
    memcpy(out, r->b + r->at, k);
    out[k] = '\0';
    r->at += k;
    return true;
}

static void line(rd_t *r, profile_line_t *l)
{
    l->mode = u8(r);
    l->dest = u8(r);
    bytes(r, l->ip, 4);
    l->rport = u16(r);
    l->lport = u16(r);
}

int profile_decode(const uint8_t *b, size_t n, profile_t *p)
{
    rd_t r = { b, n, 0, false };
    memset(p, 0, sizeof *p);
    if (u8(&r) != PROFILE_FORMAT)
        return PROFILE_E_FORMAT;
    if (!str(&r, p->name, 1, 32))
        return PROFILE_E_NAME;
    p->role = u8(&r);
    p->address = u8(&r);
    p->x1_baud = u32(&r);
    bytes(&r, p->radio, 4);
    p->ap_channel = u8(&r);
    p->ap_hidden = u8(&r);
    p->ap_max_clients = u8(&r);
    p->ap_auth = u8(&r);
    if (!str(&r, p->ap_ssid, 1, 32) || !str(&r, p->ap_pass, 0, 63))
        return PROFILE_E_FORMAT;
    bytes(&r, p->ip, 4);
    bytes(&r, p->mask, 4);
    p->dhcp = u8(&r);
    bytes(&r, p->pool_start, 4);
    bytes(&r, p->pool_end, 4);
    p->lease_min = u16(&r);
    p->offer = u8(&r);
    line(&r, &p->line[0]);
    line(&r, &p->line[1]);
    p->x2_baud = u32(&r);
    p->tx1 = (int8_t)u8(&r);
    p->rx1 = (int8_t)u8(&r);
    p->xlat1 = u8(&r);
    p->nnets = u8(&r);
    if (r.bad)
        return PROFILE_E_FORMAT;
    if (p->nnets > PROFILE_NETS)
        return PROFILE_E_NETS;
    for (int i = 0; i < p->nnets; i++)
        if (!str(&r, p->nets[i].ssid, 1, 32) || !str(&r, p->nets[i].pass, 0, 64))
            return PROFILE_E_NETS;
    if (r.bad || r.at != n)
        return PROFILE_E_FORMAT; /* short, or bytes left over */
    if (p->role > 1)
        return PROFILE_E_ROLE;
    if (p->address == 0 || p->address == 255)
        return PROFILE_E_ADDRESS;
    return PROFILE_OK;
}

typedef struct {
    uint8_t *b;
    size_t   cap, at;
    bool     full;
} wr_t;

static void w8(wr_t *w, uint8_t v)
{
    if (w->at + 1 > w->cap) {
        w->full = true;
        return;
    }
    w->b[w->at++] = v;
}

static void w16(wr_t *w, uint16_t v)
{
    w8(w, (uint8_t)v);
    w8(w, (uint8_t)(v >> 8));
}

static void w32(wr_t *w, uint32_t v)
{
    w16(w, (uint16_t)v);
    w16(w, (uint16_t)(v >> 16));
}

static void wbytes(wr_t *w, const uint8_t *v, size_t k)
{
    for (size_t i = 0; i < k; i++)
        w8(w, v[i]);
}

static void wstr(wr_t *w, const char *s, size_t max)
{
    size_t k = 0; /* strnlen is not C99: the PC test builds with -std=c99 */
    while (k < max && s[k])
        k++;
    w8(w, (uint8_t)k);
    wbytes(w, (const uint8_t *)s, k);
}

static void wline(wr_t *w, const profile_line_t *l)
{
    w8(w, l->mode);
    w8(w, l->dest);
    wbytes(w, l->ip, 4);
    w16(w, l->rport);
    w16(w, l->lport);
}

size_t profile_encode(const profile_t *p, uint8_t *out, size_t cap)
{
    wr_t w = { out, cap < PROFILE_MAX ? cap : PROFILE_MAX, 0, false };
    w8(&w, PROFILE_FORMAT);
    wstr(&w, p->name, 32);
    w8(&w, p->role);
    w8(&w, p->address);
    w32(&w, p->x1_baud);
    wbytes(&w, p->radio, 4);
    w8(&w, p->ap_channel);
    w8(&w, p->ap_hidden);
    w8(&w, p->ap_max_clients);
    w8(&w, p->ap_auth);
    wstr(&w, p->ap_ssid, 32);
    wstr(&w, p->ap_pass, 63);
    wbytes(&w, p->ip, 4);
    wbytes(&w, p->mask, 4);
    w8(&w, p->dhcp);
    wbytes(&w, p->pool_start, 4);
    wbytes(&w, p->pool_end, 4);
    w16(&w, p->lease_min);
    w8(&w, p->offer);
    wline(&w, &p->line[0]);
    wline(&w, &p->line[1]);
    w32(&w, p->x2_baud);
    w8(&w, (uint8_t)p->tx1);
    w8(&w, (uint8_t)p->rx1);
    w8(&w, p->xlat1);
    int n = p->nnets > PROFILE_NETS ? PROFILE_NETS : p->nnets;
    w8(&w, (uint8_t)n);
    for (int i = 0; i < n; i++) {
        wstr(&w, p->nets[i].ssid, 32);
        wstr(&w, p->nets[i].pass, 64);
    }
    return w.full ? 0 : w.at;
}

uint32_t profile_crc32(const uint8_t *b, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= b[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}
