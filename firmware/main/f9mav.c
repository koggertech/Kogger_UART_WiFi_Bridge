#include "f9mav.h"

#include <math.h>
#include <string.h>

#include "mavcrc.h"

#define ID_HEARTBEAT            0
#define ID_GPS_RAW_INT          24
#define ID_ATTITUDE             30
#define ID_GLOBAL_POSITION_INT  33
#define ID_BATTERY_STATUS       147
#define ID_STATUSTEXT           253

#define MAV_SEVERITY_DEBUG       7

#define MAV_TYPE_SURFACE_BOAT    11
#define MAV_AUTOPILOT_GENERIC    0  /* KoggerApp shows no boat for MAV_AUTOPILOT_INVALID (MavVehicle) */
#define MAV_STATE_ACTIVE         4
#define GPS_FIX_TYPE_NO_FIX      1
#define GPS_FIX_TYPE_3D_FIX      3

static uint16_t x25(const uint8_t *d, size_t n, uint16_t crc)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t t = (uint8_t)(d[i] ^ (crc & 0xFF));
        t = (uint8_t)(t ^ (t << 4));
        crc = (uint16_t)((crc >> 8) ^ ((uint16_t)t << 8) ^ ((uint16_t)t << 3) ^ (t >> 4));
    }
    return crc;
}

/* One MAVLink 2 frame of a known message (its length is the message's base length). */
static size_t frame(f9mav_t *m, uint32_t id, const uint8_t *pl, uint8_t *out)
{
    const mavcrc_t *e = mavcrc_find(id);
    size_t n = e->min_len;
    out[0] = 0xFD;
    out[1] = (uint8_t)n;
    out[2] = 0; /* incompat: not signed */
    out[3] = 0;
    out[4] = m->seq++;
    out[5] = F9MAV_SYSID;
    out[6] = F9MAV_COMPID;
    out[7] = (uint8_t)id;
    out[8] = (uint8_t)(id >> 8);
    out[9] = (uint8_t)(id >> 16);
    memcpy(out + 10, pl, n);
    uint16_t crc = x25(out + 1, 9 + n, 0xFFFF);
    crc = x25(&e->crc_extra, 1, crc);
    out[10 + n] = (uint8_t)crc;
    out[11 + n] = (uint8_t)(crc >> 8);
    return 12 + n;
}

static void u16le(uint8_t *b, uint16_t v)
{
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
}

static void u32le(uint8_t *b, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        b[i] = (uint8_t)(v >> (8 * i));
}

static void f32le(uint8_t *b, float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    u32le(b, u);
}

/* Half away from zero, written out so host/f9.py can do exactly the same. */
static int32_t round_i32(double x)
{
    return (int32_t)(x >= 0 ? floor(x + 0.5) : -floor(-x + 0.5));
}

static bool pos_ok(const f9_pos_t *p)
{
    /* Either coordinate exactly 0 is a placeholder, not a place: 2026-10-08 the controller sent -0.3742232 / 0.0 for
     * a second between real fixes (26-29 satellites). A real fix is practically never exactly 0.0 in float32. */
    return p->sats >= F9MAV_MIN_SATS && isfinite(p->lat) && isfinite(p->lon) && fabs(p->lat) <= 90.0 &&
           fabs(p->lon) <= 180.0 && p->lat != 0.0f && p->lon != 0.0f;
}

size_t f9mav_position(f9mav_t *m, const f9_pos_t *p, uint64_t now_us, uint8_t *out)
{
    uint8_t pl[64];
    size_t n = 0;
    uint32_t ms = (uint32_t)(now_us / 1000);
    bool pos = pos_ok(p), hdg = isfinite(p->heading);
    int32_t lat = pos ? round_i32((double)p->lat * 1e7) : 0;
    int32_t lon = pos ? round_i32((double)p->lon * 1e7) : 0;
    uint16_t cdeg = UINT16_MAX;
    if (hdg) {
        int32_t c = round_i32((double)p->heading * 100.0) % 36000;
        cdeg = (uint16_t)(c < 0 ? c + 36000 : c);
    }

    memset(pl, 0, sizeof pl); /* GPS_RAW_INT: time_usec lat lon alt eph epv vel cog fix_type sats */
    for (int i = 0; i < 8; i++)
        pl[i] = (uint8_t)(now_us >> (8 * i));
    u32le(pl + 8, (uint32_t)lat);
    u32le(pl + 12, (uint32_t)lon);
    u16le(pl + 20, UINT16_MAX);
    u16le(pl + 22, UINT16_MAX);
    u16le(pl + 24, UINT16_MAX);
    u16le(pl + 26, UINT16_MAX); /* course over ground: not known (the heading is not the course) */
    pl[28] = pos ? GPS_FIX_TYPE_3D_FIX : GPS_FIX_TYPE_NO_FIX;
    pl[29] = p->sats;
    n += frame(m, ID_GPS_RAW_INT, pl, out + n);

    if (pos) {
        memset(pl, 0, sizeof pl); /* time_boot_ms lat lon alt relative_alt vx vy vz hdg */
        u32le(pl, ms);
        u32le(pl + 4, (uint32_t)lat);
        u32le(pl + 8, (uint32_t)lon);
        u16le(pl + 26, cdeg);
        n += frame(m, ID_GLOBAL_POSITION_INT, pl, out + n);
    }

    memset(pl, 0, sizeof pl); /* current_consumed energy_consumed temperature voltages[10] current id function type remaining */
    u32le(pl, UINT32_MAX);      /* -1: not known */
    u32le(pl + 4, UINT32_MAX);
    u16le(pl + 8, INT16_MAX);   /* temperature not known */
    u16le(pl + 10, (uint16_t)(p->volt_dv * 100u));
    for (int i = 1; i < 10; i++)
        u16le(pl + 10 + 2 * i, UINT16_MAX);
    u16le(pl + 30, UINT16_MAX); /* current -1 */
    pl[35] = 0xFF;              /* remaining -1 */
    n += frame(m, ID_BATTERY_STATUS, pl, out + n);
    return n;
}

size_t f9mav_attitude(f9mav_t *m, float heading, uint64_t now_us, uint8_t *out)
{
    uint8_t pl[28];
    if (!isfinite(heading))
        return 0;
    double h = fmod((double)heading, 360.0);
    if (h > 180.0)
        h -= 360.0;
    else if (h <= -180.0)
        h += 360.0;
    float yaw = (float)(h * (3.14159265358979323846 / 180.0));
    if (fabsf(yaw) < 1e-6f)
        yaw = 1e-6f; /* KoggerApp drops an ATTITUDE whose yaw, pitch and roll are all ~0 (qFuzzyIsNull) */
    memset(pl, 0, sizeof pl); /* time_boot_ms roll pitch yaw rollspeed pitchspeed yawspeed */
    u32le(pl, (uint32_t)(now_us / 1000));
    f32le(pl + 12, yaw);
    return frame(m, ID_ATTITUDE, pl, out);
}

size_t f9mav_c0_front(f9mav_t *m, const uint8_t *c0, size_t len, uint8_t *out)
{
    static const char hex[] = "0123456789abcdef";
    float h;
    if (!f9_status_heading(c0, len, &h))
        return 0;
    uint8_t pl[51] = { MAV_SEVERITY_DEBUG }; /* severity, text[50]: not 0-terminated when full */
    char *t = (char *)pl + 1;
    size_t n = 0;
    memcpy(t, "F9 C0 front", 11);
    n = 11;
    for (int i = 2; i < 12; i++) {
        t[n++] = ' ';
        t[n++] = hex[c0[i] >> 4];
        t[n++] = hex[c0[i] & 15];
    }
    memcpy(t + n, " sats ", 6);
    n += 6;
    unsigned s = c0[17]; /* up to 3 digits: 11 + 30 + 6 + 3 = 50 at most */
    if (s >= 100)
        t[n++] = (char)('0' + s / 100);
    if (s >= 10)
        t[n++] = (char)('0' + s / 10 % 10);
    t[n++] = (char)('0' + s % 10);
    return frame(m, ID_STATUSTEXT, pl, out);
}

size_t f9mav_heartbeat(f9mav_t *m, uint8_t *out)
{
    uint8_t pl[9] = { 0, 0, 0, 0, MAV_TYPE_SURFACE_BOAT, MAV_AUTOPILOT_GENERIC, 0, MAV_STATE_ACTIVE, 3 };
    return frame(m, ID_HEARTBEAT, pl, out);
}
