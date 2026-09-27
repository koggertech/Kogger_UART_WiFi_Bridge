#include "kframe.h"

#include <string.h>

#include "mavcrc.h"

void kf_init(kf_t *k, uint8_t *buf, uint8_t *pb, size_t cap)
{
    memset(k, 0, sizeof *k);
    k->buf = buf;
    k->pb = pb;
    k->cap = cap;
    k->budget = KF_BUDGET_SAVE * cap;
}

static void raw_emit(kf_t *k, kf_cb_t cb, void *ctx)
{
    if (k->raw_len) {
        k->raw_bytes += (uint32_t)k->raw_len;
        cb(ctx, KF_RAW, k->raw, k->raw_len, 0);
        k->raw_len = 0;
    }
}

static void raw_add(kf_t *k, uint8_t b, kf_cb_t cb, void *ctx)
{
    k->raw[k->raw_len++] = b;
    if (k->raw_len == KF_RAW_RUN)
        raw_emit(k, cb, ctx);
}

/* Drop the candidate: its first byte is raw, buf[1..len) is rescanned ahead of anything queued.
 * pb_len + len <= cap always holds (queued bytes are consumed before new input), so it fits.
 * Without budget for the rescan the whole candidate goes out raw instead. */
static void reject(kf_t *k, kf_cb_t cb, void *ctx)
{
    size_t n = k->len - 1;
    if (n > k->budget) {
        for (size_t i = 0; i < k->len; i++)
            raw_add(k, k->buf[i], cb, ctx);
        k->no_rescan++;
        k->len = 0;
        k->need = 0;
        k->proto = 0;
        return;
    }
    k->budget -= n;
    raw_add(k, k->buf[0], cb, ctx);
    if (k->pb_head < n) {
        memmove(k->pb + n, k->pb + k->pb_head, k->pb_len);
        k->pb_head = n;
    }
    k->pb_head -= n;
    memcpy(k->pb + k->pb_head, k->buf + 1, n);
    k->pb_len += n;
    k->len = 0;
    k->need = 0;
    k->proto = 0;
}

static uint16_t x25(uint16_t crc, uint8_t b)
{
    uint8_t t = (uint8_t)(b ^ (uint8_t)crc);
    t = (uint8_t)(t ^ (uint8_t)(t << 4));
    return (uint16_t)((crc >> 8) ^ ((uint16_t)t << 8) ^ ((uint16_t)t << 3) ^ (t >> 4));
}

static bool fletcher_ok(const uint8_t *f, size_t n)
{
    uint8_t c1 = 0, c2 = 0;
    for (size_t i = 2; i < n - 2; i++) {
        c1 = (uint8_t)(c1 + f[i]);
        c2 = (uint8_t)(c2 + c1);
    }
    return c1 == f[n - 2] && c2 == f[n - 1];
}

static bool mav_ok(const uint8_t *f, size_t crc_at, uint8_t extra)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 1; i < crc_at; i++)
        crc = x25(crc, f[i]);
    crc = x25(crc, extra);
    return f[crc_at] == (uint8_t)crc && f[crc_at + 1] == (uint8_t)(crc >> 8);
}

/* Header complete: work out the total length, or reject an implausible header. */
static bool header(kf_t *k)
{
    const uint8_t *h = k->buf;
    switch (k->proto) {
    case KF_P_KP1:
        if (k->len < 6)
            return true;
        k->need = (size_t)h[5] + 8;
        return true;
    case KF_P_KP2:
        if (k->len < 4)
            return true;
        k->need = (size_t)h[2] | ((size_t)h[3] << 8);
        return k->need >= 7 && k->need <= k->cap;
    case KF_P_UBX:
        if (k->len < 6)
            return true;
        k->need = ((size_t)h[4] | ((size_t)h[5] << 8)) + 8;
        return k->need <= k->cap;
    case KF_P_MAV1: {
        if (k->len < 6)
            return true;
        const mavcrc_t *e = mavcrc_find(h[5]);
        if (!e || h[1] != e->min_len) /* MAVLink 1 carries exactly the base payload */
            return false;
        k->extra = e->crc_extra;
        k->need = (size_t)h[1] + 8;
        return true;
    }
    case KF_P_MAV2: {
        if (k->len < 10)
            return true;
        const mavcrc_t *e = mavcrc_find((uint32_t)h[7] | ((uint32_t)h[8] << 8) | ((uint32_t)h[9] << 16));
        if ((h[2] & ~1u) || !e || h[1] == 0 || h[1] > e->max_len)
            return false; /* unknown flags, unknown message, impossible length */
        k->extra = e->crc_extra;
        k->need = (size_t)h[1] + 12 + ((h[2] & 1u) ? 13 : 0);
        return true;
    }
    default:
        return false;
    }
}

static bool frame_ok(const kf_t *k)
{
    switch (k->proto) {
    case KF_P_KP1:
    case KF_P_KP2:
    case KF_P_UBX:
        return fletcher_ok(k->buf, k->need);
    case KF_P_MAV1:
        return mav_ok(k->buf, k->need - 2, k->extra);
    case KF_P_MAV2:
        return mav_ok(k->buf, (size_t)k->buf[1] + 10, k->extra);
    default:
        return false;
    }
}

static void step(kf_t *k, uint8_t b, kf_cb_t cb, void *ctx)
{
    if (k->len == 0) {
        switch (b) {
        case 0xBB: case 0xCC: case 0xB5: k->proto = 0; break;      /* second sync byte decides */
        case 0xFE: k->proto = KF_P_MAV1; break;
        case 0xFD: k->proto = KF_P_MAV2; break;
        default:
            raw_add(k, b, cb, ctx);
            return;
        }
        k->buf[0] = b;
        k->len = 1;
        return;
    }
    if (k->len == 1 && k->proto == 0) {
        uint8_t s = k->buf[0];
        unsigned p = (s == 0xBB && b == 0x55) ? KF_P_KP1 : (s == 0xCC && b == 0x55) ? KF_P_KP2
                   : (s == 0xB5 && b == 0x62) ? KF_P_UBX : 0;
        if (p) {
            k->proto = p;
            k->buf[1] = b;
            k->len = 2;
            return;
        }
        k->len = 0;
        raw_add(k, s, cb, ctx);
        step(k, b, cb, ctx); /* b may start a frame itself ("BB BB 55"); depth is at most 1 */
        return;
    }

    k->buf[k->len++] = b;
    if (k->need == 0 && !header(k)) {
        reject(k, cb, ctx); /* the sync was noise */
        return;
    }
    if (k->need && k->len == k->need) {
        if (frame_ok(k)) {
            raw_emit(k, cb, ctx);
            k->frames++;
            cb(ctx, KF_FRAME, k->buf, k->need, k->proto);
            k->len = 0;
            k->need = 0;
            k->proto = 0;
        } else {
            k->bad_ck++;
            reject(k, cb, ctx);
        }
    }
}

void kf_feed(kf_t *k, const uint8_t *data, size_t n, kf_cb_t cb, void *ctx)
{
    size_t i = 0;
    for (;;) {
        uint8_t b;
        if (k->pb_len) {
            b = k->pb[k->pb_head++];
            if (--k->pb_len == 0)
                k->pb_head = 0;
        } else if (i < n) {
            b = data[i++];
            if (k->budget < KF_BUDGET_SAVE * k->cap)
                k->budget += KF_BUDGET_RATIO;
        } else {
            break;
        }
        step(k, b, cb, ctx);
    }
}

void kf_flush(kf_t *k, bool force, kf_cb_t cb, void *ctx)
{
    /* An overdue candidate is not released wholesale: real frames may sit behind a false sync whose
     * announced length never arrived. Give up its first byte and rescan the rest, until nothing is
     * pending; every round moves at least one byte to raw, so this ends. */
    while (force && k->len) {
        reject(k, cb, ctx);
        kf_feed(k, NULL, 0, cb, ctx);
    }
    raw_emit(k, cb, ctx);
}
