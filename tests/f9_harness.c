/*
 * f9_harness.c - firmware/main/f9.c and f9mav.c on the PC, driven by tests/test_f9.py.
 * stdin, one command a line:
 *   S <hex>               feed bytes to the stream parser      -> "F <hex>" for every frame decided
 *   E                     end of the data                       -> "F <hex>" ...
 *   C                     counters                              -> "N <frames> <skipped>"
 *   P <hex> <now_us> <seq>  frames for one F9 C4                -> "M <hex>" (or "M -" if no C4) "Q <next seq>"
 *   A <hex> <now_us> <seq>  ATTITUDE for one F9 C0              -> "M <hex>" (or "M -" if no C0 / no frame) "Q <next seq>"
 *   T <hex> <seq>         STATUSTEXT with the front of one F9 C0 -> "M <hex>" (or "M -") "Q <next seq>"
 *   H <seq>               one heartbeat                         -> "M <hex>" "Q <next seq>"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "f9.h"
#include "f9mav.h"

static void hex_out(const char *tag, const uint8_t *d, size_t n)
{
    printf("%s ", tag);
    for (size_t i = 0; i < n; i++)
        printf("%02x", d[i]);
    printf("\n");
}

static size_t hex_in(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (s[0] && s[1] && n < cap) {
        unsigned v;
        if (sscanf(s, "%2x", &v) != 1)
            break;
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

static void on_frame(void *ctx, const uint8_t *f, size_t len)
{
    (void)ctx;
    hex_out("F", f, len);
}

int main(void)
{
    static char line[200000];
    static uint8_t buf[100000];
    f9_t p;
    f9_init(&p);
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == 'S') {
            size_t n = hex_in(line + 2, buf, sizeof buf);
            f9_feed(&p, buf, n, on_frame, NULL);
        } else if (line[0] == 'E') {
            f9_end(&p, on_frame, NULL);
        } else if (line[0] == 'C') {
            printf("N %lu %lu\n", (unsigned long)p.frames, (unsigned long)p.skipped);
        } else if (line[0] == 'P') {
            char hex[128];
            unsigned long long now;
            unsigned seq;
            if (sscanf(line + 2, "%127s %llu %u", hex, &now, &seq) != 3)
                return 2;
            size_t n = hex_in(hex, buf, sizeof buf);
            f9_pos_t pos;
            f9mav_t m = { (uint8_t)seq };
            uint8_t out[F9MAV_MAX];
            if (f9_pos(buf, n, &pos))
                hex_out("M", out, f9mav_position(&m, &pos, now, out));
            else
                printf("M -\n");
            printf("Q %u\n", m.seq);
        } else if (line[0] == 'A') {
            char hex[128];
            unsigned long long now;
            unsigned seq;
            if (sscanf(line + 2, "%127s %llu %u", hex, &now, &seq) != 3)
                return 2;
            size_t n = hex_in(hex, buf, sizeof buf);
            float h;
            f9mav_t m = { (uint8_t)seq };
            uint8_t out[F9MAV_MAX];
            size_t k = f9_status_heading(buf, n, &h) ? f9mav_attitude(&m, h, now, out) : 0;
            if (k)
                hex_out("M", out, k);
            else
                printf("M -\n");
            printf("Q %u\n", m.seq);
        } else if (line[0] == 'T') {
            char hex[128];
            unsigned seq;
            if (sscanf(line + 2, "%127s %u", hex, &seq) != 2)
                return 2;
            size_t n = hex_in(hex, buf, sizeof buf);
            f9mav_t m = { (uint8_t)seq };
            uint8_t out[F9MAV_MAX];
            size_t k = f9mav_c0_front(&m, buf, n, out);
            if (k)
                hex_out("M", out, k);
            else
                printf("M -\n");
            printf("Q %u\n", m.seq);
        } else if (line[0] == 'H') {
            f9mav_t m = { (uint8_t)atoi(line + 2) };
            uint8_t out[F9MAV_MAX];
            hex_out("M", out, f9mav_heartbeat(&m, out));
            printf("Q %u\n", m.seq);
        }
        fflush(stdout);
    }
    return 0;
}
