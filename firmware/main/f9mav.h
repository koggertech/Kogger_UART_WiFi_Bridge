/*
 * f9mav.h - MAVLink 2 frames built from F9 C4 (position) telemetry, so a ground station that speaks MAVLink
 * (KoggerApp: map, track, mosaic) sees the boat without changes (docs/GODIO_F9.md).
 *
 * A position is valid only with coordinates in range, neither of them exactly 0, AND at least F9MAV_MIN_SATS satellites: the
 * controller keeps sending its last (or a factory) position without a fix - seen 2026-10-07 with 1 satellite and a
 * frozen position 1000 km away - and KoggerApp takes GLOBAL_POSITION_INT as it is, without looking at a fix.
 * Per F9 C4 (position; 1 Hz, 4.8 Hz since the controller update of 2026-10-07), in this order:
 *   GPS_RAW_INT (#24)          fix 3 with a valid position, else 1 (no fix) and lat/lon 0; satellites
 *   GLOBAL_POSITION_INT (#33)  only with a valid position; lat/lon deg x 1e7; hdg = heading x 100 (cdeg, 0..35999)
 *   BATTERY_STATUS (#147)      voltages[0] = supply voltage in mV, the rest "unknown"
 * Per F9 C0 (status; 8 Hz, 4.8 Hz since 2026-10-07; the same heading): the only source of attitude, so not two
 *   ATTITUDE (#30)             only with a finite heading; yaw = heading in rad, wrapped to (-pi, pi]
 *                              (the mosaic takes its course from here, not from #33.hdg); |yaw| < 1e-6 is sent
 *                              as 1e-6: KoggerApp ignores an ATTITUDE that is all ~0
 * and HEARTBEAT (#0): surface boat, autopilot GENERIC (KoggerApp shows no vehicle for INVALID), active - sent
 * by the caller once a second.
 * Base payload lengths (no extensions, no truncation of trailing zeros); CRC_EXTRA from mavcrc.
 * Portable C99 (tests/test_f9.py vs host/f9.py, byte for byte).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "f9.h"

#ifdef __cplusplus
extern "C" {
#endif

#define F9MAV_SYSID   1
#define F9MAV_COMPID  1
#define F9MAV_MAX     256   /**< room for everything one f9mav_position() call writes */
#define F9MAV_MIN_SATS 4    /**< fewer satellites: no 3D fix (Inference: byte 17 counts satellites in the fix,
                                  its exact meaning is still to be checked on water) */

typedef struct {
    uint8_t seq;            /**< MAVLink sequence, one per frame */
} f9mav_t;

/** Frames for one F9 C4 into out (F9MAV_MAX bytes); returns the bytes written. now_us: monotonic time. */
size_t f9mav_position(f9mav_t *m, const f9_pos_t *p, uint64_t now_us, uint8_t *out);

/** ATTITUDE for the heading of one F9 C0 into out (F9MAV_MAX bytes); 0 when the heading is not finite. */
size_t f9mav_attitude(f9mav_t *m, float heading, uint64_t now_us, uint8_t *out);

/** STATUSTEXT (#253, severity DEBUG) "F9 C0 front 00 01 .. 00 sats 38": bytes 2..11 of a valid F9 C0 (status,
 *  GPS speed per the vendor; a fix flag is looked for there) and its satellites, for finding their meaning from a
 *  capture of the line's UDP or KoggerApp's message log. The caller sends it when those bytes change. 0 when c0
 *  is no valid F9 C0. */
size_t f9mav_c0_front(f9mav_t *m, const uint8_t *c0, size_t len, uint8_t *out);

/** One HEARTBEAT frame into out (F9MAV_MAX bytes); returns its length. */
size_t f9mav_heartbeat(f9mav_t *m, uint8_t *out);

#ifdef __cplusplus
}
#endif
