#include "linexlat.h"

#include <string.h>

#include "f9.h"
#include "f9mav.h"
#include "relay.h"

#define HEARTBEAT_US  1000000
#define ALIVE_US      5000000   /* heartbeats only while F9 frames come: no phantom boat when the tap is off */

/* Only the line 1 RX task calls in: no locking. */
static f9_t s_f9;
static f9mav_t s_mav;
static uint8_t s_out[F9MAV_MAX];
static int64_t s_last_frame_us = -ALIVE_US, s_last_hb_us = -HEARTBEAT_US;
static bool s_init;
static uint8_t s_c0_front[10];
static bool s_c0_seen, s_c0_sats_ok;

static void on_frame(void *ctx, const uint8_t *f, size_t len)
{
    int64_t now = *(const int64_t *)ctx;
    f9_pos_t p;
    float heading;
    if (f[0] != 0xF9)
        return; /* F8 commands are not ours to translate (a tap on the other wire) */
    s_last_frame_us = now;
    if (f9_pos(f, len, &p)) /* position, satellites, voltage */
        relay_line_bytes(1, s_out, f9mav_position(&s_mav, &p, (uint64_t)now, s_out));
    else if (f9_status_heading(f, len, &heading)) { /* the heading, the only source of ATTITUDE */
        relay_line_bytes(1, s_out, f9mav_attitude(&s_mav, heading, (uint64_t)now, s_out));
        /* where is the fix flag? (docs): on a change of the front, and when the satellites cross the threshold */
        bool sats_ok = f[17] >= F9MAV_MIN_SATS;
        if (!s_c0_seen || memcmp(s_c0_front, f + 2, sizeof s_c0_front) || sats_ok != s_c0_sats_ok) {
            memcpy(s_c0_front, f + 2, sizeof s_c0_front);
            s_c0_seen = true;
            s_c0_sats_ok = sats_ok;
            relay_line_bytes(1, s_out, f9mav_c0_front(&s_mav, f, len, s_out));
        }
    }
}

static void init_once(void)
{
    if (!s_init) {
        f9_init(&s_f9);
        s_init = true;
    }
}

void linexlat_bytes(const uint8_t *d, size_t n, int64_t now_us)
{
    init_once();
    f9_feed(&s_f9, d, n, on_frame, &now_us);
}

void linexlat_idle(int64_t now_us)
{
    init_once();
    f9_end(&s_f9, on_frame, &now_us);
    if (now_us - s_last_frame_us < ALIVE_US && now_us - s_last_hb_us >= HEARTBEAT_US) {
        s_last_hb_us = now_us;
        relay_line_bytes(1, s_out, f9mav_heartbeat(&s_mav, s_out));
    }
}
