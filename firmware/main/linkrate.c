#include "linkrate.h"

#include <string.h>

/* 802.11b/g PHY rate codes 0x00..0x0F (wifi_phy_rate_t, the codes wifi_pkt_rx_ctrl_t.rate carries), kbit/s. */
static const uint32_t BG_KBPS[16] = {
    1000, 2000, 5500, 11000, 0, 2000, 5500, 11000,
    48000, 24000, 12000, 6000, 54000, 36000, 18000, 9000,
};

/* 802.11n, one spatial stream, MCS0..MCS7: [40 MHz][short GI][MCS], kbit/s. */
static const uint32_t HT_KBPS[2][2][8] = {
    { { 6500, 13000, 19500, 26000, 39000, 52000, 58500, 65000 },
      { 7200, 14400, 21700, 28900, 43300, 57800, 65000, 72200 } },
    { { 13500, 27000, 40500, 54000, 81000, 108000, 121500, 135000 },
      { 15000, 30000, 45000, 60000, 90000, 120000, 135000, 150000 } },
};

typedef struct {
    uint8_t  kind, code, flags;
    uint16_t frames;
    uint32_t bound; /* kbit/s, the largest timing bound of this rate in the window */
} rate_t;

typedef struct {
    uint8_t  mac[6];
    bool     lr;
    uint32_t frames;
    int32_t  rssi_sum;
    uint8_t  nrates;
    rate_t   rates[LINKRATE_RATES_MAX];
    int      last;     /* rate index of the last timed frame, -1 = none */
    uint32_t last_ts;
    uint16_t last_len;
} peer_t;

static peer_t   s_peer[LINKRATE_PEERS_MAX];
static int      s_n;
static uint16_t s_window;
static uint8_t  s_phy = 0xFF;

uint32_t linkrate_kbps(uint8_t kind, uint8_t code, uint8_t flags)
{
    switch (kind) {
    case LINKRATE_K_B:
        return code <= 0x07 ? BG_KBPS[code] : 0;
    case LINKRATE_K_G:
        return code >= 0x08 && code <= 0x0F ? BG_KBPS[code] : 0;
    case LINKRATE_K_HT:
        return code < 8 ? HT_KBPS[(flags & LINKRATE_F_40) ? 1 : 0][(flags & LINKRATE_F_SGI) ? 1 : 0][code] : 0;
    default:
        return 0; /* LR and other: no documented meaning of the fields (linkrate.h) */
    }
}

void linkrate_classify(uint8_t sig_mode, uint8_t rate, uint8_t mcs, bool cwb, bool sgi, bool lr,
                       uint8_t *kind, uint8_t *code, uint8_t *flags)
{
    *flags = 0;
    if (sig_mode == 1) {
        *kind = LINKRATE_K_HT;
        *code = mcs;
        *flags = (uint8_t)((sgi ? LINKRATE_F_SGI : 0) | (cwb ? LINKRATE_F_40 : 0));
    } else if (sig_mode == 0) {
        *kind = lr ? LINKRATE_K_LR : rate <= 0x07 ? LINKRATE_K_B : LINKRATE_K_G;
        *code = rate;
    } else {
        *kind = LINKRATE_K_OTHER;
        *code = sig_mode;
    }
}

uint16_t linkrate_clamp_window(uint16_t ms)
{
    if (ms == 0)
        return LINKRATE_WINDOW_DEF;
    if (ms < LINKRATE_WINDOW_MIN)
        return LINKRATE_WINDOW_MIN;
    return ms > LINKRATE_WINDOW_MAX ? LINKRATE_WINDOW_MAX : ms;
}

void linkrate_begin(const uint8_t peers[][6], const bool *lr, int n, uint16_t window_ms, uint8_t phy_mode)
{
    memset(s_peer, 0, sizeof s_peer);
    s_n = n < 0 ? 0 : n > LINKRATE_PEERS_MAX ? LINKRATE_PEERS_MAX : n;
    for (int i = 0; i < s_n; i++) {
        memcpy(s_peer[i].mac, peers[i], 6);
        s_peer[i].lr = lr && lr[i];
        s_peer[i].last = -1;
    }
    s_window = window_ms;
    s_phy = phy_mode;
}

/* Two consecutive frames of one transmitter at one rate: the air time of one of them lies between the timestamps. */
static void timed(peer_t *p, int idx, uint32_t ts, uint16_t len)
{
    if (p->last == idx) {
        uint32_t dt = ts - p->last_ts; /* wraps right */
        if (dt >= LINKRATE_DT_MIN_US && dt <= LINKRATE_DT_MAX_US) {
            uint32_t bits = (uint32_t)(len < p->last_len ? len : p->last_len) * 8u;
            uint32_t kbps = (uint32_t)((uint64_t)bits * 1000u / dt);
            if (kbps > p->rates[idx].bound)
                p->rates[idx].bound = kbps;
        }
    }
    p->last = idx;
    p->last_ts = ts;
    p->last_len = len;
}

void linkrate_frame(const uint8_t *addr2, int8_t rssi, uint8_t sig_mode, uint8_t rate, uint8_t mcs, bool cwb,
                    bool sgi, uint32_t ts_us, uint16_t len, bool aggregated)
{
    peer_t *p = NULL;
    for (int i = 0; i < s_n && !p; i++)
        if (!memcmp(s_peer[i].mac, addr2, 6))
            p = &s_peer[i];
    if (!p)
        return;
    uint8_t kind, code, flags;
    linkrate_classify(sig_mode, rate, mcs, cwb, sgi, p->lr, &kind, &code, &flags);
    p->frames++;
    p->rssi_sum += rssi;
    int idx = -1;
    for (int i = 0; i < p->nrates && idx < 0; i++) {
        rate_t *r = &p->rates[i];
        if (r->kind == kind && r->code == code && r->flags == flags) {
            if (r->frames < 0xFFFF)
                r->frames++;
            idx = i;
        }
    }
    if (idx < 0 && p->nrates < LINKRATE_RATES_MAX) {
        p->rates[p->nrates] = (rate_t){ kind, code, flags, 1, 0 };
        idx = p->nrates++;
    }
    if (aggregated)
        return; /* the parts of an aggregate share a timestamp: no pair can be timed with them */
    if (idx < 0)
        p->last = -1; /* a rate not listed breaks the chain */
    else
        timed(p, idx, ts_us, len);
}

int linkrate_total(void)
{
    return s_n;
}

static void put16(uint8_t *o, uint16_t v)
{
    o[0] = (uint8_t)v;
    o[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *o, uint32_t v)
{
    put16(o, (uint16_t)v);
    put16(o + 2, (uint16_t)(v >> 16));
}

int linkrate_page(int index, uint8_t *out)
{
    if (index < 0 || index >= s_n)
        return 0;
    const peer_t *p = &s_peer[index];
    int order[LINKRATE_RATES_MAX];
    for (int i = 0; i < p->nrates; i++)
        order[i] = i;
    for (int i = 1; i < p->nrates; i++) /* the most frames first; equal counts keep the order they came in */
        for (int j = i; j > 0 && p->rates[order[j]].frames > p->rates[order[j - 1]].frames; j--) {
            int t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    int top = p->nrates < LINKRATE_TOP ? p->nrates : LINKRATE_TOP;
    out[0] = (uint8_t)index;
    out[1] = (uint8_t)s_n;
    memcpy(out + 2, p->mac, 6);
    put16(out + 8, s_window);
    put16(out + 10, p->frames > 0xFFFF ? 0xFFFF : (uint16_t)p->frames);
    out[12] = (uint8_t)(int8_t)(p->frames ? p->rssi_sum / (int32_t)p->frames : -128);
    out[13] = s_phy;
    out[14] = (uint8_t)top;
    out[15] = 0;
    for (int i = 0; i < top; i++) {
        const rate_t *r = &p->rates[order[i]];
        uint8_t *e = out + LINKRATE_PAGE_HEAD + i * LINKRATE_ENTRY;
        uint8_t flags = r->flags;
        uint32_t kbps = linkrate_kbps(r->kind, r->code, r->flags);
        if (r->kind == LINKRATE_K_LR && r->bound >= LINKRATE_LR_PROOF_KBPS && r->bound <= LINKRATE_LR_CAP_KBPS) {
            kbps = LINKRATE_LR_FAST_KBPS; /* only 250 and 500 exist, and 250 cannot be timed above 250 */
            flags |= LINKRATE_F_TIMED;
        }
        e[0] = r->kind;
        e[1] = r->code;
        e[2] = flags;
        put32(e + 3, kbps);
        put16(e + 7, r->frames);
        put32(out + LINKRATE_PAGE_HEAD + top * LINKRATE_ENTRY + i * LINKRATE_BOUND, r->bound);
    }
    return LINKRATE_PAGE_HEAD + top * (LINKRATE_ENTRY + LINKRATE_BOUND);
}
