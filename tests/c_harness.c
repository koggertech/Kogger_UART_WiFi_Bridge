/*
 * Host harness for the portable firmware sources (frame.c, proto.c, sbp.c).
 * Driven by tests/test_all.py, which checks the output against the Python mirrors
 * (host/wbframe.py, host/sbpframe.py).
 *
 *   c_harness enc            -> "type len hex" per vector (vectors from an LCG shared with the test)
 *   c_harness dec FILE       -> "F type hex" per decoded frame, then "S ok crc discarded"
 *   c_harness pct            -> stdin lines of hex bytes -> "E encoded" / "D hex-of-decode" per line
 *   c_harness parse          -> stdin lines -> "P rc tag cmd argc | args..." per line
 *   c_harness sbpenc         -> "route mode id len hex" per SBP vector (LCG seed 777)
 *   c_harness sbpdec FILE    -> "F route mode id ck1 ck2 hex" per SBP frame, then "S ok check_errors"
 *   c_harness relay FILE CAP -> relay framing: "U kind flags hex" per unit, "P hex" per packet,
 *                               then "S frames bad_ck raw_bytes"
 *   c_harness portinfo       -> stdin script (clock N | up/down PORT HEX | rx/tx PORT N | mrx/mtx PORT |
 *                               p0 PORT FLAGS SLIP | p1 PORT): units cut by kframe as in the firmware,
 *                               "P0 hex" / "P1 hex" per page
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bootkey.h"
#include "frame.h"
#include "kframe.h"
#include "kpack.h"
#include "portinfo.h"
#include "proto.h"
#include "sbp.h"
#include "linkrate.h"
#include "survey.h"

static uint32_t lcg = 12345;
static uint8_t rnd(void)
{
    lcg = lcg * 1103515245u + 12345u;
    return (uint8_t)(lcg >> 16);
}

static void hex(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        printf("%02x", p[i]);
}

static void on_frame(void *ctx, uint8_t type, const uint8_t *p, size_t n)
{
    (void)ctx;
    printf("F %u ", type);
    hex(p, n);
    printf("\n");
}

static void on_sbp(void *ctx, const sbp_frame_t *f)
{
    (void)ctx;
    printf("F %u %u %u %u %u ", f->route, f->mode, f->id, f->ck1, f->ck2);
    hex(f->payload, f->len);
    printf("\n");
}

static void on_packet(void *ctx, const uint8_t *pkt, size_t len)
{
    (void)ctx;
    printf("P ");
    hex(pkt, len);
    printf("\n");
}

static void on_unit(void *ctx, kf_kind_t kind, const uint8_t *d, size_t len, unsigned flags)
{
    kp_t *p = ctx;
    printf("U %d %u ", (int)kind, flags);
    hex(d, len);
    printf("\n");
    kp_unit(p, d, len, on_packet, NULL);
}

static uint32_t pi_clock = 1;

static uint32_t pi_now(void)
{
    return pi_clock;
}

static void on_pi_up(void *ctx, kf_kind_t kind, const uint8_t *d, size_t len, unsigned flags)
{
    portinfo_up((int)(intptr_t)ctx, portinfo_proto(kind, flags), d, len);
}

static void on_pi_down(void *ctx, kf_kind_t kind, const uint8_t *d, size_t len, unsigned flags)
{
    portinfo_down((int)(intptr_t)ctx, portinfo_proto(kind, flags), d, len);
}

static int unhex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (s[0] && s[1] && s[0] != '\n' && s[0] != '\r') {
        unsigned v;
        if (sscanf(s, "%2x", &v) != 1 || n >= cap)
            return -1;
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return (int)n;
}

int main(int argc, char **argv)
{
    static uint8_t buf[FRAME_ENCODED_MAX(FRAME_MAX_PAYLOAD)];
    static uint8_t pl[FRAME_MAX_PAYLOAD];
    if (argc >= 3 && !strcmp(argv[1], "bootkey")) { /* argv[2]: one poll per char, d = down */
        bootkey_t k = {0};
        for (const char *p = argv[2]; *p; p++) {
            bootkey_act_t a = bootkey_step(&k, *p == 'd');
            putchar(a == BOOTKEY_RESET_ALL ? 'R' : a == BOOTKEY_RESET_RATES ? 'r' : a == BOOTKEY_LED_ON ? '*'
                    : a == BOOTKEY_LED_DARK ? '-' : '.');
        }
        putchar('\n');
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "survey")) {
        static char line[512], hx[64];
        static uint8_t page[SURVEY_PAGE_LEN], mac[6], bss[6], own[6];
        static char hx2[64];
        while (fgets(line, sizeof line, stdin)) {
            char cmd[16];
            unsigned a = 0, b = 0, c = 0, d = 0, e = 0;
            int r = 0, nz = 0;
            if (sscanf(line, "%15s", cmd) != 1)
                continue;
            if (!strcmp(cmd, "air")) { /* sig_mode rate mcs sgi len */
                sscanf(line, "%*s %u %u %u %u %u", &a, &b, &c, &d, &e);
                printf("A %lu\n", (unsigned long)survey_airtime_us((uint8_t)a, (uint8_t)b, (uint8_t)c, d != 0,
                                                                   (uint16_t)e));
            } else if (!strcmp(cmd, "clamp")) { /* dwell mask */
                sscanf(line, "%*s %u %u", &a, &b);
                printf("C %u %u\n", survey_clamp_dwell((uint16_t)a), survey_clamp_mask((uint16_t)b));
            } else if (!strcmp(cmd, "begin")) { /* mask dwell home [own bssid] */
                hx[0] = '\0';
                sscanf(line, "%*s %u %u %u %63s", &a, &b, &c, hx);
                int n = hx[0] ? unhex(hx, own, sizeof own) : 0;
                survey_begin(survey_clamp_mask((uint16_t)a), (uint16_t)b, (uint8_t)c, n == 6 ? own : NULL);
            } else if (!strcmp(cmd, "dwell")) { /* channel ms */
                sscanf(line, "%*s %u %u", &a, &b);
                survey_dwell_done((uint8_t)a, (uint16_t)b);
            } else if (!strcmp(cmd, "frame")) { /* ch rssi noise air_us [addr2] [bssid] */
                hx[0] = hx2[0] = '\0';
                sscanf(line, "%*s %u %d %d %u %63s %63s", &a, &r, &nz, &b, hx, hx2);
                int n = hx[0] ? unhex(hx, mac, sizeof mac) : 0;
                int m = hx2[0] ? unhex(hx2, bss, sizeof bss) : 0;
                survey_frame((uint8_t)a, (int8_t)r, (int8_t)nz, b, n == 6 ? mac : NULL, m == 6 ? bss : NULL);
            } else if (!strcmp(cmd, "pages")) {
                printf("N %d\n", survey_total());
                for (int i = 0; survey_page(i, page); i++) {
                    printf("P ");
                    hex(page, sizeof page);
                    printf("\n");
                }
            }
        }
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "linkrate")) {
        static char line[512], hx[64];
        static uint8_t page[LINKRATE_PAGE_MAX], peers[LINKRATE_PEERS_MAX][6], mac[6];
        static bool lr[LINKRATE_PEERS_MAX];
        int np = 0;
        while (fgets(line, sizeof line, stdin)) {
            char cmd[16];
            unsigned a = 0, b = 0, c = 0, d = 0, e = 0, g = 0;
            int r = 0;
            if (sscanf(line, "%15s", cmd) != 1)
                continue;
            if (!strcmp(cmd, "kbps")) { /* kind code flags */
                sscanf(line, "%*s %u %u %u", &a, &b, &c);
                printf("K %lu\n", (unsigned long)linkrate_kbps((uint8_t)a, (uint8_t)b, (uint8_t)c));
            } else if (!strcmp(cmd, "class")) { /* sig_mode rate mcs cwb sgi lr */
                sscanf(line, "%*s %u %u %u %u %u %u", &a, &b, &c, &d, &e, &g);
                uint8_t k, co, fl;
                linkrate_classify((uint8_t)a, (uint8_t)b, (uint8_t)c, d != 0, e != 0, g != 0, &k, &co, &fl);
                printf("T %u %u %u\n", k, co, fl);
            } else if (!strcmp(cmd, "clamp")) {
                sscanf(line, "%*s %u", &a);
                printf("W %u\n", linkrate_clamp_window((uint16_t)a));
            } else if (!strcmp(cmd, "peer")) { /* mac lr */
                sscanf(line, "%*s %63s %u", hx, &a);
                if (np < LINKRATE_PEERS_MAX && unhex(hx, peers[np], 6) == 6)
                    lr[np++] = a != 0;
            } else if (!strcmp(cmd, "begin")) { /* window phy */
                sscanf(line, "%*s %u %u", &a, &b);
                linkrate_begin((const uint8_t (*)[6])peers, lr, np, (uint16_t)a, (uint8_t)b);
            } else if (!strcmp(cmd, "frame")) { /* mac rssi sig_mode rate mcs cwb sgi */
                sscanf(line, "%*s %63s %d %u %u %u %u %u", hx, &r, &a, &b, &c, &d, &e);
                if (unhex(hx, mac, 6) == 6)
                    linkrate_frame(mac, (int8_t)r, (uint8_t)a, (uint8_t)b, (uint8_t)c, d != 0, e != 0);
            } else if (!strcmp(cmd, "pages")) {
                printf("N %d\n", linkrate_total());
                for (int i = 0;; i++) {
                    int len = linkrate_page(i, page);
                    if (!len)
                        break;
                    printf("P ");
                    hex(page, (size_t)len);
                    printf("\n");
                }
            }
        }
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "portinfo")) {
        static uint8_t fb[4][2][1024];
        static kf_t kf[4]; /* up X1, up X2, down X1, down X2 */
        static char line[20000], hx[20000];
        static uint8_t data[8192], out[256];
        portinfo_init(pi_now, NULL, NULL);
        for (int i = 0; i < 4; i++)
            kf_init(&kf[i], fb[i][0], fb[i][1], sizeof fb[i][0]);
        while (fgets(line, sizeof line, stdin)) {
            char cmd[16];
            int port = 0;
            unsigned a = 0, b = 0;
            if (sscanf(line, "%15s", cmd) != 1)
                continue;
            if (!strcmp(cmd, "clock")) {
                sscanf(line, "%*s %u", &a);
                pi_clock = a;
            } else if (!strcmp(cmd, "up") || !strcmp(cmd, "down")) {
                bool up = cmd[0] == 'u';
                if (sscanf(line, "%*s %d %19999s", &port, hx) != 2 || port < 0 || port > 1)
                    return 4;
                int n = unhex(hx, data, sizeof data);
                if (n < 0)
                    return 3;
                kf_t *k = &kf[(up ? 0 : 2) + port];
                kf_feed(k, data, (size_t)n, up ? on_pi_up : on_pi_down, (void *)(intptr_t)port);
                kf_flush(k, true, up ? on_pi_up : on_pi_down, (void *)(intptr_t)port);
            } else if (!strcmp(cmd, "rx") || !strcmp(cmd, "tx")) {
                sscanf(line, "%*s %d %u", &port, &a);
                if (cmd[0] == 'r')
                    portinfo_rx_bytes(port, a);
                else
                    portinfo_tx_bytes(port, a);
            } else if (!strcmp(cmd, "mrx") || !strcmp(cmd, "mtx")) {
                sscanf(line, "%*s %d", &port);
                if (cmd[1] == 'r')
                    portinfo_module_rx(port);
                else
                    portinfo_module_tx(port);
            } else if (!strcmp(cmd, "p0")) {
                sscanf(line, "%*s %d %u %u", &port, &a, &b);
                size_t n = portinfo_page0(port, (uint8_t)a, 921600, 115200, 7, 9, b != 0, out);
                printf("P0 ");
                hex(out, n);
                printf("\n");
            } else if (!strcmp(cmd, "p1")) {
                sscanf(line, "%*s %d", &port);
                size_t n = portinfo_page1(port, out);
                printf("P1 ");
                hex(out, n);
                printf("\n");
            }
        }
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "enc")) {
        static const size_t lens[] = { 0, 1, 2, 3, 7, 64, 255, 1000, 1500 };
        for (size_t v = 0; v < sizeof lens / sizeof lens[0]; v++) {
            uint8_t type = (uint8_t)(v % 2 ? FRAME_T_CTL : FRAME_T_IP);
            for (size_t i = 0; i < lens[v]; i++) {
                uint8_t r = rnd();
                pl[i] = (r & 7) == 0 ? FRAME_END : (r & 7) == 1 ? FRAME_ESC : r;
            }
            size_t n = frame_encode(type, pl, lens[v], buf, sizeof buf);
            printf("%u %u ", type, (unsigned)lens[v]);
            hex(buf, n);
            printf("\n");
        }
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "dec")) {
        FILE *f = fopen(argv[2], "rb");
        if (!f)
            return 2;
        static uint8_t dbuf[FRAME_MAX_PAYLOAD + 3];
        frame_dec_t d;
        frame_dec_init(&d, dbuf, sizeof dbuf);
        uint8_t chunk[37]; /* odd chunk size exercises frames split across reads */
        size_t n;
        while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
            frame_dec_feed(&d, chunk, n, on_frame, NULL);
        fclose(f);
        printf("S %u %u %u\n", (unsigned)d.frames_ok, (unsigned)d.crc_errors, (unsigned)d.discarded);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "pct")) {
        char line[4096], enc[4096];
        uint8_t raw[1024], dec[1024];
        while (fgets(line, sizeof line, stdin)) {
            int n = unhex(line, raw, sizeof raw);
            if (n < 0)
                return 3;
            int e = pct_encode(raw, (size_t)n, enc, sizeof enc);
            printf("E %s\n", e < 0 ? "<overflow>" : enc);
            int k = pct_decode(enc, dec, sizeof dec);
            printf("D ");
            if (k < 0)
                printf("<error>");
            else
                hex(dec, (size_t)k);
            printf("\n");
        }
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "parse")) {
        char line[1024];
        while (fgets(line, sizeof line, stdin)) {
            line[strcspn(line, "\r\n")] = '\0';
            proto_line_t l;
            int rc = proto_parse(line, &l);
            if (rc != 0) {
                printf("P -1\n");
                continue;
            }
            printf("P 0 %s %s %d |", l.tag, l.cmd, l.argc);
            for (int i = 0; i < l.argc; i++)
                printf(" %s", l.argv[i]);
            const char *s = proto_arg(&l, "ssid");
            printf(" | ssid=%s\n", s ? s : "<none>");
        }
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "sbpenc")) {
        static const unsigned lens[] = { 0, 1, 2, 7, 64, 200, 255 };
        static uint8_t out[SBP_FRAME_MAX];
        uint32_t st = 777;
        for (unsigned v = 0; v < sizeof lens / sizeof lens[0]; v++) {
            uint8_t p2[SBP_MAX_PAYLOAD];
            for (unsigned i = 0; i < lens[v]; i++) {
                st = st * 1103515245u + 12345u;
                uint8_t r = (uint8_t)(st >> 16);
                p2[i] = (r & 7) == 0 ? SBP_SYNC1 : (r & 7) == 1 ? SBP_SYNC2 : r;
            }
            uint8_t route = (uint8_t)(v * 37u), id = (uint8_t)(0x57u ^ v);
            uint8_t mode = SBP_MODE(v % 3 + 1, v % 8, v & 1, (v >> 1) & 1);
            size_t n = sbp_encode(route, mode, id, p2, (uint8_t)lens[v], out);
            printf("%u %u %u %u ", route, mode, id, lens[v]);
            hex(out, n);
            printf("\n");
        }
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "sbpdec")) {
        FILE *f = fopen(argv[2], "rb");
        if (!f)
            return 2;
        static sbp_dec_t d;
        sbp_dec_init(&d);
        uint8_t chunk[29]; /* odd chunk size: frames split across reads */
        size_t n;
        while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
            sbp_dec_feed(&d, chunk, n, on_sbp, NULL);
        fclose(f);
        printf("S %u %u\n", (unsigned)d.frames_ok, (unsigned)d.check_errors);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "relay")) {
        FILE *f = fopen(argv[2], "rb");
        if (!f)
            return 2;
        size_t cap = (size_t)atoi(argv[3]);
        uint8_t *fbuf = malloc(cap), *fpb = malloc(cap);
        static kf_t k;
        static kp_t p;
        kf_init(&k, fbuf, fpb, cap);
        kp_init(&p, KP_MAX_PACKET);
        uint8_t chunk[41]; /* odd size: frames split across reads */
        size_t n;
        unsigned nchunk = 0;
        while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) {
            kf_feed(&k, chunk, n, on_unit, &p);
            if (++nchunk % 24 == 0) { /* short idle: a pending candidate is kept */
                kf_flush(&k, false, on_unit, &p);
                kp_flush(&p, on_packet, NULL);
            }
        }
        kf_flush(&k, true, on_unit, &p); /* long silence at the end: release everything */
        kp_flush(&p, on_packet, NULL);
        fclose(f);
        printf("S %u %u %u\n", (unsigned)k.frames, (unsigned)k.bad_ck, (unsigned)k.raw_bytes);
        free(fbuf);
        free(fpb);
        return 0;
    }
    fprintf(stderr, "usage: see file header\n");
    return 1;
}
