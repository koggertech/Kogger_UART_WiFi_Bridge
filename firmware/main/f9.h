/*
 * f9.h - GODIO receiver <-> controller serial frames (F8 commands, F9 telemetry), cut out of a byte stream
 * (docs/GODIO_F9.md).
 *
 * A frame has no length field: sync (F8/F9), type, data, and an 8-bit sum of all previous bytes. The
 * boundary rule is the one of the reference decoder (host/f9.py iter_frames): at a sync byte the
 * shortest length F9_MIN_LEN..F9_MAX_LEN whose sum is valid AND that is followed by another sync (or by the end
 * of the data) is the frame; a sync with no such length is skipped as noise. Fed in pieces, the parser decides
 * a frame only once the byte after it (or the end of the data: f9_end) is known, so the frames found are those
 * the reference finds in the whole stream, whatever the split. Portable C99 (tests/test_f9.py vs host/f9.py).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define F9_MIN_LEN 4
#define F9_MAX_LEN 40
#define F9_BUF     (F9_MAX_LEN + 1)   /**< a sync, its longest candidate and the byte after it */

typedef void (*f9_cb_t)(void *ctx, const uint8_t *frame, size_t len);

typedef struct {
    uint8_t  buf[F9_BUF];
    size_t   len;
    uint32_t frames, skipped;          /**< frames found; bytes skipped as noise */
} f9_t;

void f9_init(f9_t *p);

/** Bytes of the stream; every frame that can be decided is handed to cb. */
void f9_feed(f9_t *p, const uint8_t *d, size_t n, f9_cb_t cb, void *ctx);

/** End of the data (the line went quiet): what is left is decided as the reference does at the end. */
void f9_end(f9_t *p, f9_cb_t cb, void *ctx);

/** F9 C4, the position frame (19 bytes; layout verified on hardware 2026-10-07). */
typedef struct {
    float    lon, lat;     /**< degrees */
    uint16_t dist_m;       /**< distance from the start point */
    uint8_t  volt_dv;      /**< supply voltage, 0.1 V */
    float    heading;      /**< degrees 0..360, float32 (not uint32 as the vendor example implied) */
    uint8_t  sats;
} f9_pos_t;

#define F9_C4_LEN 19
#define F9_C0_LEN 19

/** True and *out filled when frame is a valid F9 C4. */
bool f9_pos(const uint8_t *frame, size_t len, f9_pos_t *out);

/** F9 C0, the status frame (19 bytes, 8 Hz on the boat, measured 2026-10-07): it ends like C4 (voltage at 12, heading
 *  float32 at 13..16, satellites at 17); its front (status, GPS speed) is not decoded. True and *heading filled
 *  when frame is a valid F9 C0. */
bool f9_status_heading(const uint8_t *frame, size_t len, float *heading);

#ifdef __cplusplus
}
#endif
