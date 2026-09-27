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
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frame.h"
#include "kframe.h"
#include "kpack.h"
#include "proto.h"
#include "sbp.h"

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
