/*
 * kpack.h - packs kframe units into network packets of at most `max` bytes (512 for the relay):
 * whole units are never split, a unit that does not fit starts a new packet, and a unit longer
 * than `max` is cut into max-sized pieces. The concatenation of all packets equals the
 * concatenation of the units. Portable C99, host-tested.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KP_MAX_PACKET 512

typedef void (*kp_send_t)(void *ctx, const uint8_t *pkt, size_t len);

typedef struct {
    uint8_t  buf[KP_MAX_PACKET];
    size_t   len;
    size_t   max;
    uint32_t packets;
    uint32_t cuts;       /**< packets that carry only a piece of a unit longer than max */
} kp_t;

/** max <= KP_MAX_PACKET */
void kp_init(kp_t *p, size_t max);

/** A whole unit (frame or raw run). */
void kp_unit(kp_t *p, const uint8_t *d, size_t n, kp_send_t send, void *ctx);

/** Send what is collected (line idle). */
void kp_flush(kp_t *p, kp_send_t send, void *ctx);

#ifdef __cplusplus
}
#endif
