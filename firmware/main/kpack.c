#include "kpack.h"

#include <string.h>

void kp_init(kp_t *p, size_t max)
{
    memset(p, 0, sizeof *p);
    p->max = max > KP_MAX_PACKET ? KP_MAX_PACKET : max;
}

void kp_flush(kp_t *p, kp_send_t send, void *ctx)
{
    if (p->len) {
        p->packets++;
        send(ctx, p->buf, p->len);
        p->len = 0;
    }
}

void kp_unit(kp_t *p, const uint8_t *d, size_t n, kp_send_t send, void *ctx)
{
    if (n <= p->max - p->len) {
        memcpy(p->buf + p->len, d, n);
        p->len += n;
        return;
    }
    kp_flush(p, send, ctx);
    if (n <= p->max) {
        memcpy(p->buf, d, n);
        p->len = n;
        return;
    }
    while (n) { /* longer than a packet: cut into max-sized pieces, each sent on its own */
        size_t c = n < p->max ? n : p->max;
        p->packets++;
        p->cuts++;
        send(ctx, d, c);
        d += c;
        n -= c;
    }
}
