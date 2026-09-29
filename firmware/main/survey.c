/*
 * survey.c - channel busy accounting (survey.h).
 */
#include "survey.h"

#include <string.h>

/* 11b/g rates of wifi_phy_rate_t, kbit/s. 0x00..0x03 are 11b with a long preamble, 0x05..0x07 the same
 * rates with a short one, 0x08..0x0F are OFDM (11g). 0x04 is not used by the chip. */
static const uint16_t RATE_KBPS[16] = {
    1000, 2000, 5500, 11000, 0, 2000, 5500, 11000,
    48000, 24000, 12000, 6000, 54000, 36000, 18000, 9000,
};

/* Data bits in one OFDM symbol, MCS0..MCS7 at 20 MHz. */
static const uint16_t HT20_BITS[8] = { 26, 52, 78, 104, 156, 208, 234, 260 };

static uint32_t ceil_div(uint32_t a, uint32_t b)
{
    return b ? (a + b - 1) / b : 0;
}

uint32_t survey_airtime_us(uint8_t sig_mode, uint8_t rate, uint8_t mcs, bool sgi, uint16_t len)
{
    uint32_t bits = 8u * len;
    if (sig_mode == 0) {
        uint32_t kbps = RATE_KBPS[rate & 0x0F];
        if (!kbps)
            return 0;
        if (rate <= 0x07) /* DSSS: a preamble, then the frame at its own rate */
            return (rate <= 0x03 ? 192u : 96u) + ceil_div(bits * 1000u, kbps);
        /* OFDM: 20 us of preamble, then whole symbols of 4 us; 22 bits of SERVICE and tail go with it */
        return 20u + 4u * ceil_div(bits + 22u, kbps / 250u);
    }
    if (sig_mode != 1)
        return 0; /* 11ac and later: this chip does not receive them */
    /* 11n at 20 MHz: 36 us of preamble, then symbols of 4 us, or 3.6 us with a short guard interval */
    uint32_t sym = ceil_div(bits + 22u, HT20_BITS[mcs < 8 ? mcs : 7]);
    return (360u + sym * (sgi ? 36u : 40u) + 9u) / 10u;
}

uint16_t survey_clamp_dwell(uint16_t ms)
{
    if (ms == 0)
        return SURVEY_DWELL_DEF;
    if (ms < SURVEY_DWELL_MIN)
        return SURVEY_DWELL_MIN;
    return ms > SURVEY_DWELL_MAX ? SURVEY_DWELL_MAX : ms;
}

uint16_t survey_clamp_mask(uint16_t mask)
{
    /* Channels 12 and 13 are dropped: the driver's country does not let the radio visit them, and a
     * channel that was never visited must not be reported as an empty one. */
    const uint16_t all = (uint16_t)((1u << SURVEY_CH_SCAN_MAX) - 1);
    mask &= all;
    return mask ? mask : all;
}

typedef struct {
    uint32_t air_us;
    uint16_t frames;
    int8_t   rssi_max;
    uint8_t  nsend;
    uint32_t send[SURVEY_SENDERS_MAX];
    int32_t  noise_sum;
    uint16_t noise_n;
    int8_t   noise_min, noise_max;
} chan_t;

static chan_t   s_ch[SURVEY_CH_MAX];               /* the promiscuous callback's */
static uint16_t s_dwell[SURVEY_CH_MAX];            /* the manager's */
static uint8_t  s_home;

static uint32_t hash6(const uint8_t *a)
{
    uint32_t h = 2166136261u; /* FNV-1a: only used to tell transmitters apart */
    for (int i = 0; i < 6; i++) {
        h ^= a[i];
        h *= 16777619u;
    }
    return h;
}

void survey_begin(uint16_t mask, uint16_t dwell_ms, uint8_t home)
{
    (void)mask;
    (void)dwell_ms;
    memset(s_ch, 0, sizeof s_ch);
    memset(s_dwell, 0, sizeof s_dwell);
    for (int i = 0; i < SURVEY_CH_MAX; i++)
        s_ch[i].rssi_max = -128;
    s_home = home;
}

void survey_dwell_done(uint8_t channel, uint16_t ms)
{
    if (channel < SURVEY_CH_MIN || channel > SURVEY_CH_MAX)
        return;
    uint32_t sum = (uint32_t)s_dwell[channel - 1] + ms;
    s_dwell[channel - 1] = (uint16_t)(sum > 0xFFFF ? 0xFFFF : sum);
}

void survey_frame(uint8_t channel, int8_t rssi, int8_t noise, uint32_t air_us, const uint8_t *addr2)
{
    if (channel < SURVEY_CH_MIN || channel > SURVEY_CH_MAX)
        return;
    chan_t *c = &s_ch[channel - 1];
    c->air_us += air_us;
    if (c->frames < 0xFFFF)
        c->frames++;
    if (rssi > c->rssi_max)
        c->rssi_max = rssi;
    if (noise) { /* 0 means the radio gave no figure */
        if (!c->noise_n || noise < c->noise_min)
            c->noise_min = noise;
        if (!c->noise_n || noise > c->noise_max)
            c->noise_max = noise;
        c->noise_sum += noise;
        c->noise_n++;
    }
    if (!addr2)
        return;
    uint32_t h = hash6(addr2);
    for (int i = 0; i < c->nsend; i++)
        if (c->send[i] == h)
            return;
    if (c->nsend < SURVEY_SENDERS_MAX)
        c->send[c->nsend++] = h;
}

int survey_total(void)
{
    int n = 0;
    for (int i = 0; i < SURVEY_CH_MAX; i++)
        if (s_dwell[i])
            n++;
    return n;
}

/* True when the noise floor never moved during the whole survey: then it says nothing about a channel. */
static bool noise_flat(void)
{
    int8_t lo = 0, hi = 0;
    bool any = false;
    for (int i = 0; i < SURVEY_CH_MAX; i++) {
        if (!s_ch[i].noise_n)
            continue;
        if (!any || s_ch[i].noise_min < lo)
            lo = s_ch[i].noise_min;
        if (!any || s_ch[i].noise_max > hi)
            hi = s_ch[i].noise_max;
        any = true;
    }
    return !any || lo == hi;
}

bool survey_page(int index, uint8_t *out)
{
    int total = survey_total();
    if (index < 0 || index >= total)
        return false;
    int ch = -1;
    for (int i = 0, k = 0; i < SURVEY_CH_MAX; i++)
        if (s_dwell[i] && k++ == index) {
            ch = i;
            break;
        }
    const chan_t *c = &s_ch[ch];
    uint16_t dwell = s_dwell[ch];
    uint32_t busy = c->air_us / dwell; /* air time per mille: us heard / (ms * 1000) * 1000 */
    if (busy > 1000)
        busy = 1000;
    uint8_t flags = 0;
    if (s_home == ch + 1)
        flags |= SURVEY_F_HOME;
    if (ch + 1 <= SURVEY_CH_AP_MAX)
        flags |= SURVEY_F_AP_OK;
    if (noise_flat())
        flags |= SURVEY_F_NOISE_BAD;
    int noise = c->noise_n ? (c->noise_sum - (int32_t)c->noise_n / 2) / (int32_t)c->noise_n : 0;
    out[0] = (uint8_t)index;
    out[1] = (uint8_t)total;
    out[2] = (uint8_t)(ch + 1);
    out[3] = (uint8_t)dwell;
    out[4] = (uint8_t)(dwell >> 8);
    out[5] = (uint8_t)busy;
    out[6] = (uint8_t)(busy >> 8);
    out[7] = (uint8_t)c->frames;
    out[8] = (uint8_t)(c->frames >> 8);
    out[9] = c->nsend;
    out[10] = 0;
    out[11] = (uint8_t)c->rssi_max;
    out[12] = (uint8_t)(int8_t)noise;
    out[13] = flags;
    return true;
}
