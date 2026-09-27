#include "sbp.h"

size_t sbp_encode(uint8_t route, uint8_t mode, uint8_t id, const uint8_t *payload, uint8_t len, uint8_t *out)
{
    out[0] = SBP_SYNC1;
    out[1] = SBP_SYNC2;
    out[2] = route;
    out[3] = mode;
    out[4] = id;
    out[5] = len;
    for (unsigned i = 0; i < len; i++)
        out[6 + i] = payload[i];
    uint8_t c1 = 0, c2 = 0;
    for (unsigned i = 2; i < 6u + len; i++) {
        c1 = (uint8_t)(c1 + out[i]);
        c2 = (uint8_t)(c2 + c1);
    }
    out[6 + len] = c1;
    out[7 + len] = c2;
    return (size_t)len + 8;
}

void sbp_dec_init(sbp_dec_t *d)
{
    d->st = 0;
    d->frames_ok = d->check_errors = 0;
}

static void add(sbp_dec_t *d, uint8_t b)
{
    d->c1 = (uint8_t)(d->c1 + b);
    d->c2 = (uint8_t)(d->c2 + d->c1);
}

void sbp_dec_feed(sbp_dec_t *d, const uint8_t *data, size_t n, sbp_cb_t cb, void *ctx)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t b = data[i];
        switch (d->st) {
        case 0:
            if (b == SBP_SYNC1)
                d->st = 1;
            break;
        case 1: /* "BB BB 55" must still sync on the second BB */
            d->st = (b == SBP_SYNC2) ? 2 : (b == SBP_SYNC1) ? 1 : 0;
            break;
        case 2:
            d->route = b;
            d->c1 = d->c2 = 0;
            add(d, b);
            d->st = 3;
            break;
        case 3:
            d->mode = b;
            add(d, b);
            d->st = 4;
            break;
        case 4:
            d->id = b;
            add(d, b);
            d->st = 5;
            break;
        case 5:
            d->len = b;
            add(d, b);
            d->idx = 0;
            d->st = b ? 6 : 7;
            break;
        case 6:
            d->pl[d->idx++] = b;
            add(d, b);
            if (d->idx >= d->len)
                d->st = 7;
            break;
        case 7:
            d->ck1 = b;
            d->st = 8;
            break;
        case 8:
            d->st = 0;
            if (d->ck1 == d->c1 && b == d->c2) {
                d->frames_ok++;
                sbp_frame_t f = { d->route, d->mode, d->id, d->len, d->pl, d->ck1, b };
                cb(ctx, &f);
            } else {
                d->check_errors++;
            }
            break;
        }
    }
}
