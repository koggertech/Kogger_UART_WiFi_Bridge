/*
 * sbp.h - Kogger SBP (binary protocol, KP1 framing) codec.
 *
 * Frame: 0xBB 0x55 | route | mode | id | len | payload[len] | ck1 ck2
 *   mode = type[1:0] | version[5:3] | mark[6] | resp[7]
 *   type: 1 CONTENT, 2 SETTING, 3 GETTING
 *   ck1/ck2: Fletcher-8 over route..payload (bytes 2 .. 5+len), as in KoggerApp ProtoBinOut::end().
 * Multi-byte fields are little-endian. Contract of the Wi-Fi module: docs/SBP_WIFI.md.
 *
 * Portable C99, no ESP-IDF dependencies: compiled by the host harness (tests/c_harness.c) and
 * checked against host/sbpframe.py.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SBP_SYNC1        0xBB
#define SBP_SYNC2        0x55
#define SBP_MAX_PAYLOAD  255
#define SBP_FRAME_MAX    (SBP_MAX_PAYLOAD + 8)

#define SBP_T_CONTENT    1
#define SBP_T_SETTING    2
#define SBP_T_GETTING    3

/* Response codes (KoggerApp enum Resp). */
#define SBP_RESP_OK            1
#define SBP_RESP_ERR_CHECK     2
#define SBP_RESP_ERR_PAYLOAD   3
#define SBP_RESP_ERR_ID        4
#define SBP_RESP_ERR_VERSION   5
#define SBP_RESP_ERR_TYPE      6
#define SBP_RESP_ERR_KEY       7
#define SBP_RESP_ERR_RUNTIME   8

/** Confirmation key required by dangerous settings (KoggerApp IDBin::m_key). */
#define SBP_KEY_CONFIRM  0xC96B5D4Au

#define SBP_MODE(type, ver, resp, mark) \
    ((uint8_t)(((type) & 3u) | (((ver) & 7u) << 3) | (((mark) & 1u) << 6) | (((resp) & 1u) << 7)))

static inline uint8_t sbp_type(uint8_t mode) { return (uint8_t)(mode & 3u); }
static inline uint8_t sbp_ver(uint8_t mode)  { return (uint8_t)((mode >> 3) & 7u); }
static inline bool    sbp_resp(uint8_t mode) { return ((mode >> 7) & 1u) != 0; }

typedef struct {
    uint8_t        route;
    uint8_t        mode;
    uint8_t        id;
    uint8_t        len;
    const uint8_t *payload;
    uint8_t        ck1, ck2;   /**< checksum bytes as received; echoed in acknowledgements */
} sbp_frame_t;

/** Encode one frame; out must hold len + 8 bytes. Returns the frame length (len + 8). */
size_t sbp_encode(uint8_t route, uint8_t mode, uint8_t id, const uint8_t *payload, uint8_t len, uint8_t *out);

typedef void (*sbp_cb_t)(void *ctx, const sbp_frame_t *f);

typedef struct {
    uint8_t  st, route, mode, id, len, idx, c1, c2, ck1;
    uint8_t  pl[SBP_MAX_PAYLOAD];
    uint32_t frames_ok;
    uint32_t check_errors;
} sbp_dec_t;

void sbp_dec_init(sbp_dec_t *d);

/** Feed received bytes; cb runs synchronously for every frame whose checksum matches. */
void sbp_dec_feed(sbp_dec_t *d, const uint8_t *data, size_t n, sbp_cb_t cb, void *ctx);

/* Little-endian field helpers. */
static inline void     sbp_put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void     sbp_put_u32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static inline uint16_t sbp_get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t sbp_get_u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

#ifdef __cplusplus
}
#endif
