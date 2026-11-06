#include "f9.h"

#include <string.h>

static bool is_sync(uint8_t b)
{
    return b == 0xF8 || b == 0xF9;
}

void f9_init(f9_t *p)
{
    memset(p, 0, sizeof *p);
}

static void drop(f9_t *p, size_t n)
{
    memmove(p->buf, p->buf + n, p->len - n);
    p->len -= n;
}

/* Decide what can be decided at the head of the buffer. Without `end` a length is decided only when the byte
 * after it is there, so a frame is never taken (or refused) before the reference could tell. */
static void run(f9_t *p, bool end, f9_cb_t cb, void *ctx)
{
    while (p->len) {
        if (!is_sync(p->buf[0])) {
            drop(p, 1);
            p->skipped++;
            continue;
        }
        size_t found = 0, k = 0;
        uint8_t sum = 0;
        for (size_t l = F9_MIN_LEN; l <= F9_MAX_LEN; l++) {
            if (l > p->len) {
                if (!end)
                    return; /* the shorter lengths failed for good; this one needs more bytes */
                break;
            }
            while (k + 1 < l)
                sum = (uint8_t)(sum + p->buf[k++]); /* sum of bytes 0..l-2 */
            if (sum != p->buf[l - 1])
                continue;
            if (l == p->len) {
                if (!end)
                    return; /* valid so far: the next byte (or the end) decides */
                found = l;
                break;
            }
            if (is_sync(p->buf[l])) {
                found = l;
                break;
            }
        }
        if (found) {
            cb(ctx, p->buf, found);
            p->frames++;
            drop(p, found);
        } else {
            drop(p, 1);
            p->skipped++;
        }
    }
}

void f9_feed(f9_t *p, const uint8_t *d, size_t n, f9_cb_t cb, void *ctx)
{
    for (size_t i = 0; i < n; i++) {
        p->buf[p->len++] = d[i]; /* run() leaves at most F9_MAX_LEN bytes: there is always room for one */
        run(p, false, cb, ctx);
    }
}

void f9_end(f9_t *p, f9_cb_t cb, void *ctx)
{
    run(p, true, cb, ctx);
}

static float f32le(const uint8_t *b)
{
    uint32_t u = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

static bool sum_ok(const uint8_t *f, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i + 1 < len; i++)
        sum = (uint8_t)(sum + f[i]);
    return sum == f[len - 1];
}

bool f9_status_heading(const uint8_t *f, size_t len, float *heading)
{
    if (len != F9_C0_LEN || f[0] != 0xF9 || f[1] != 0xC0 || !sum_ok(f, len))
        return false;
    *heading = f32le(f + 13);
    return true;
}

bool f9_pos(const uint8_t *f, size_t len, f9_pos_t *out)
{
    if (len != F9_C4_LEN || f[0] != 0xF9 || f[1] != 0xC4 || !sum_ok(f, len))
        return false;
    out->lon = f32le(f + 2);
    out->lat = f32le(f + 6);
    out->dist_m = (uint16_t)(f[10] | f[11] << 8);
    out->volt_dv = f[12];
    out->heading = f32le(f + 13);
    out->sats = f[17];
    return true;
}
