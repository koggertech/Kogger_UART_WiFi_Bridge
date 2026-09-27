/*
 * kframe.h - splits a byte stream into units for the relay (docs/RELAY.md):
 *   KF_FRAME  one complete frame whose checksum is correct, "from SYNC to CRC":
 *               Kogger KP1  BB 55 route mode id len ...          Fletcher-8
 *               Kogger KP2  CC 55 U2(total) ...                  Fletcher-8
 *               u-blox UBX  B5 62 class id U2(len) ...           Fletcher-8
 *               MAVLink 1   FE len seq sys comp id ...           X.25 + CRC_EXTRA (known ids, len = base length)
 *               MAVLink 2   FD len incompat compat seq sys comp id[3] ... [+13 signature]
 *                                                                X.25 + CRC_EXTRA (known ids, len <= max length)
 *   KF_RAW    bytes that are not part of such a frame (other protocols, noise, a failed candidate),
 *             delivered in runs of up to KF_RAW_RUN bytes; scanning restarts at the byte after a
 *             failed sync.
 * A frame is only emitted after its checksum was verified, so a false sync inside data cannot swallow
 * the real frames behind it: it fails and those bytes are rescanned. Frames are held whole, up to
 * `cap` bytes; longer announced lengths are treated as noise.
 * Nothing is dropped and the order is kept: the concatenation of all units equals the input.
 * Portable C99 (host-tested by tests/test_all.py against host/kframe.py).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { KF_FRAME = 1, KF_RAW = 2 } kf_kind_t;

/* Protocol of a KF_FRAME, in the low bits of `flags`. */
#define KF_P_MASK   7u
#define KF_P_KP1    1u
#define KF_P_KP2    2u
#define KF_P_UBX    3u
#define KF_P_MAV1   4u
#define KF_P_MAV2   5u

#define KF_RAW_RUN  512  /**< raw bytes are delivered in runs of up to this size (= one packet) */
#define KF_MIN_CAP  280  /**< the longest KP1 (263) and signed MAVLink 2 (280) frames must fit */

/** Rescan budget: every new input byte allows KF_BUDGET_RATIO rescanned bytes, saved up to
 *  KF_BUDGET_SAVE x cap. A failed candidate is rescanned only while the budget covers it, otherwise it
 *  goes out raw whole. Random data needs ~0.1x, the sync-heavy test streams 5-7x in bursts; a stream
 *  built of false syncs would need ~700x and is cut to ~9x (docs/RELAY.md). */
#define KF_BUDGET_RATIO 8
#define KF_BUDGET_SAVE  32

typedef void (*kf_cb_t)(void *ctx, kf_kind_t kind, const uint8_t *data, size_t len, unsigned flags);

typedef struct {
    uint8_t *buf;          /**< candidate frame, cap bytes */
    uint8_t *pb;           /**< bytes to rescan after a failed candidate, cap bytes */
    size_t   cap;
    size_t   len;          /**< bytes in buf */
    size_t   need;         /**< total length of the candidate once known, else 0 */
    unsigned proto;        /**< KF_P_* of the candidate once its sync is complete */
    uint8_t  extra;        /**< MAVLink CRC_EXTRA of the candidate */
    size_t   pb_head, pb_len;
    size_t   budget;       /**< bytes that may still be rescanned */
    uint8_t  raw[KF_RAW_RUN];
    size_t   raw_len;
    uint32_t frames, bad_ck, raw_bytes;
    uint32_t no_rescan;    /**< failed candidates released raw because the budget was spent */
} kf_t;

/** buf and pb must each hold cap bytes, cap >= KF_MIN_CAP (largest frame accepted). */
void kf_init(kf_t *k, uint8_t *buf, uint8_t *pb, size_t cap);

void kf_feed(kf_t *k, const uint8_t *data, size_t n, kf_cb_t cb, void *ctx);

/**
 * Line went quiet. Pending raw bytes are delivered. With `force` (long silence) an unfinished
 * candidate is given up too: its rest is overdue and holding it would stall the data. Its bytes are
 * rescanned (first byte raw), so complete frames behind a false sync are still found.
 */
void kf_flush(kf_t *k, bool force, kf_cb_t cb, void *ctx);

/** True while a frame candidate is being collected (a soft flush then keeps it). */
static inline bool kf_pending(const kf_t *k) { return k->len != 0; }

#ifdef __cplusplus
}
#endif
