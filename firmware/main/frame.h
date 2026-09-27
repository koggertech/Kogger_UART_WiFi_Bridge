/*
 * frame.h - link framing between the ESP32-C3 bridge and the host.
 *
 * Wire format (docs/PROTOCOL.md, section 2):
 *   0xC0 | SLIP-escaped( type[1] | payload[n] | crc16_be[2] ) | 0xC0
 * CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over type+payload.
 *
 * Portable C99, no ESP-IDF dependencies: the same file is compiled by the
 * host test harness (tests/c_harness.c) and checked against host/wbframe.py.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FRAME_END      0xC0
#define FRAME_ESC      0xDB
#define FRAME_ESC_END  0xDC
#define FRAME_ESC_ESC  0xDD

#define FRAME_T_IP     0x01  /**< payload = one IPv4 packet */
#define FRAME_T_CTL    0x02  /**< payload = one control line, UTF-8, no newline */

#define FRAME_MAX_PAYLOAD 1500

/** Worst-case encoded size for a payload of n bytes (every byte escaped + 2 delimiters). */
#define FRAME_ENCODED_MAX(n) (2u * ((size_t)(n) + 3u) + 2u)

/** CRC-16/CCITT-FALSE; pass 0xFFFF as crc for a fresh computation. */
uint16_t frame_crc16(const uint8_t *data, size_t len, uint16_t crc);

/**
 * Encode one frame into out.
 * @return number of bytes written, or 0 if out_cap is too small
 *         (FRAME_ENCODED_MAX(len) is always sufficient).
 */
size_t frame_encode(uint8_t type, const uint8_t *payload, size_t len,
                    uint8_t *out, size_t out_cap);

/** Called for every frame whose CRC matched. */
typedef void (*frame_cb_t)(void *ctx, uint8_t type, const uint8_t *payload, size_t len);

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    bool     esc;
    bool     bad;          /**< current frame is being discarded (overflow / bad escape) */
    uint32_t frames_ok;
    uint32_t crc_errors;   /**< non-empty frames with CRC mismatch or shorter than 3 bytes */
    uint32_t discarded;    /**< frames dropped for overflow or invalid escape */
} frame_dec_t;

/** buf must hold type + FRAME_MAX_PAYLOAD + crc, i.e. at least FRAME_MAX_PAYLOAD + 3 bytes. */
void frame_dec_init(frame_dec_t *d, uint8_t *buf, size_t cap);

/** Feed raw received bytes; cb is invoked synchronously for each valid frame. */
void frame_dec_feed(frame_dec_t *d, const uint8_t *data, size_t len, frame_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
