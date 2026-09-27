#include "frame.h"

uint16_t frame_crc16(const uint8_t *data, size_t len, uint16_t crc)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

static size_t put_escaped(uint8_t *out, size_t pos, uint8_t c)
{
    if (c == FRAME_END) {
        out[pos++] = FRAME_ESC;
        out[pos++] = FRAME_ESC_END;
    } else if (c == FRAME_ESC) {
        out[pos++] = FRAME_ESC;
        out[pos++] = FRAME_ESC_ESC;
    } else {
        out[pos++] = c;
    }
    return pos;
}

size_t frame_encode(uint8_t type, const uint8_t *payload, size_t len,
                    uint8_t *out, size_t out_cap)
{
    if (out_cap < FRAME_ENCODED_MAX(len))
        return 0;
    uint16_t crc = frame_crc16(&type, 1, 0xFFFF);
    crc = frame_crc16(payload, len, crc);

    size_t pos = 0;
    out[pos++] = FRAME_END;
    pos = put_escaped(out, pos, type);
    for (size_t i = 0; i < len; i++)
        pos = put_escaped(out, pos, payload[i]);
    pos = put_escaped(out, pos, (uint8_t)(crc >> 8));
    pos = put_escaped(out, pos, (uint8_t)crc);
    out[pos++] = FRAME_END;
    return pos;
}

void frame_dec_init(frame_dec_t *d, uint8_t *buf, size_t cap)
{
    d->buf = buf;
    d->cap = cap;
    d->len = 0;
    d->esc = false;
    d->bad = false;
    d->frames_ok = d->crc_errors = d->discarded = 0;
}

static void finish(frame_dec_t *d, frame_cb_t cb, void *ctx)
{
    if (d->bad || d->esc) { /* esc: frame ended right after an escape byte */
        d->discarded++;
    } else if (d->len > 0) {
        if (d->len < 3) {
            d->crc_errors++;
        } else {
            size_t body = d->len - 2;
            uint16_t want = (uint16_t)((d->buf[body] << 8) | d->buf[body + 1]);
            if (frame_crc16(d->buf, body, 0xFFFF) == want) {
                d->frames_ok++;
                cb(ctx, d->buf[0], d->buf + 1, body - 1);
            } else {
                d->crc_errors++;
            }
        }
    }
    d->len = 0;
    d->esc = false;
    d->bad = false;
}

void frame_dec_feed(frame_dec_t *d, const uint8_t *data, size_t len, frame_cb_t cb, void *ctx)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        if (c == FRAME_END) {
            finish(d, cb, ctx);
            continue;
        }
        if (d->bad)
            continue;
        if (d->esc) {
            d->esc = false;
            if (c == FRAME_ESC_END)
                c = FRAME_END;
            else if (c == FRAME_ESC_ESC)
                c = FRAME_ESC;
            else {
                d->bad = true;
                continue;
            }
        } else if (c == FRAME_ESC) {
            d->esc = true;
            continue;
        }
        if (d->len >= d->cap) {
            d->bad = true;
            continue;
        }
        d->buf[d->len++] = c;
    }
}
