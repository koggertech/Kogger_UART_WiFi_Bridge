/*
 * gps_sniffer - listen-only UART identifier for the Kogger Wi-Fi board (ESP32-C3-MINI-1U),
 * updatable over its own X1 serial port without the BOOT button (two OTA slots and rollback, as in the
 * bridge firmware; the update dialect is this tool's own, see ota_push.py).
 *
 * Purpose: find the pinout (which X2 pin is the device's TX) and the baud rate of an unknown serial
 * line, by trying UART1 RX on each X2 pin, at a list of baud rates, inverted and not, and scoring the
 * bytes for valid NMEA-0183 (a checksummed "$...*HH" sentence) or u-blox UBX (sync B5 62 + Fletcher).
 * Built for tapping a spare connector of a boat's GPS module that repeats the module's output, so the
 * line into the autopilot is never touched.
 *
 * SAFETY - listen-only by construction: UART1 TX is parked on GPIO10 (not on any header) and the code
 * never writes to UART1, so no X2 pin is ever driven. Do NOT connect the GPS VCC to the board; connect
 * only GND (first) and the two signal pins to X2. Power the board from the computer side.
 *
 * X1 (UART0, 115200 8N1) is owned by this app, not by the IDF console:
 *   out: banner, sweep table, "#BOOT", "#LOCK" every 3 s, relayed NMEA, raw "#RAW" dumps, "#OTA" replies;
 *   in:  commands, one per line:
 *          "#KOGG-OTA <size> <crc32hex>"  start an update (ota_push.py);
 *          "#LISTEN <pin> <baud>"         dump one pin raw: pin 4 = X2.2 / 5 = X2.1; baud 0 = sweep rates
 *                                         and lock onto the first that gives valid frames, announcing
 *                                         "#FOUND ... GODIO-F9 (telemetry)" / "GODIO-F8 (command)" / NMEA;
 *          "#SNIFF"                        back to the auto sweep (default; locks on GODIO/NMEA/UBX).
 * Use "#LISTEN <pin> 0" on an unknown line (receiver<->autopilot controller) to identify its rate and
 * direction; the raw dump feeds host/f9.py (frames_of). Listen-only: the line is only read, never driven.
 * LED (GPIO0): on = powered and running; toggles with relayed/raw data; toggles per update chunk.
 *
 * Board pinout (docs/HARDWARE.md, netcfg.c schematic note):
 *   X1: pin1 TX = GPIO21, pin2 RX = GPIO20, pin3 GND, pin4 VCC in
 *   X2: pin1 = GPIO5 (series R7), pin2 = GPIO4 (series R11), pin3 GND, pin4 VCC (leave off)
 *
 * Wiring, build, first flash and updates: tools/gps_sniffer/README.md.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SNIFFER_VERSION   "0.6"

#define CON_UART          UART_NUM_0   /* X1, to the computer */
#define CON_TX_GPIO       21
#define CON_RX_GPIO       20
#define CON_BAUD          115200
#define SNIFF_UART        UART_NUM_1   /* X2, to the device under test */
#define TX_PARK_GPIO      10           /* not on any header: UART1 TX idles here, never on X2 */
#define LED_GPIO          0            /* board LED VD1, lit when high */

#define RX_BUF            4096
#define CAP               4096         /* bytes captured per probe */
#define PRELISTEN_MS      180          /* quick carrier check before committing a full window */
#define WINDOW_MS         1500         /* long enough to catch one NMEA burst at 9600 */
#define LOCK_EVERY_US     3000000      /* "#LOCK" repeat period, so any capture window sees it */
#define RESWEEP_MS        5000         /* pause before sweeping again when nothing locked */

#define OTA_CHUNK_MAX     4096
#define OTA_IDLE_MS       5000         /* no complete chunk for this long aborts an update */
#define CONFIRM_AFTER_US  3000000      /* a freshly updated image confirms itself after running this long */

/* The two signal pins brought out on connector X2. The device's TX lands on one of them. */
static const int RX_PINS[] = {4, 5};
static const char *rx_label(int pin) { return pin == 4 ? "X2.2" : pin == 5 ? "X2.1" : "?"; }

/* Common GNSS/serial rates, most likely first (GPS modules default to 9600, 38400 or 115200). */
static const uint32_t BAUDS[] = {9600, 38400, 115200, 4800, 19200, 57600, 230400, 460800, 921600};

typedef struct {
    int pin;
    uint32_t baud;
    bool invert;
    size_t bytes;
    int printable_pct;
    bool nmea;              /* a sentence with a valid XOR checksum was seen */
    bool ubx;               /* a UBX message with a valid Fletcher checksum was seen */
    uint8_t godio;          /* 0xF9/0xF8 if valid GODIO frames were seen (telemetry/command), else 0 */
    int godio_cnt;
    char sample[72];        /* first readable run, for the human watching */
} probe_t;

/* ---------------------------------------------------------------- LED and output */

static int s_led;
static volatile bool s_ota_active;   /* an update owns X1: the sniffer stays silent */

/* Runtime mode, set over X1 by con_task; the long-running loops return when s_mode_gen changes. */
typedef enum { MODE_SWEEP, MODE_LISTEN } mode_t;
static volatile mode_t s_mode = MODE_SWEEP;
static volatile int s_listen_pin = 5;      /* X2.1 = the reverse line into the GPS, by default */
static volatile int s_listen_baud = 115200;
static volatile uint32_t s_mode_gen;       /* bumped on every mode change request */

static void led_set(int on) { s_led = on; gpio_set_level(LED_GPIO, on); }
static void led_toggle(void) { led_set(!s_led); }

static void con_write(const void *d, size_t n)
{
    uart_write_bytes(CON_UART, d, n);
}

static void vsay(bool force, const char *fmt, va_list ap)
{
    if (s_ota_active && !force)
        return;
    char b[256];
    int n = vsnprintf(b, sizeof b, fmt, ap);
    if (n > 0)
        con_write(b, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1);
}

/** Sniffer text: dropped while an update owns X1. */
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsay(false, fmt, ap);
    va_end(ap);
}

/** Update replies: always sent. */
static void ota_say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsay(true, fmt, ap);
    va_end(ap);
}

/* ---------------------------------------------------------------- CRC-32 (zlib / ISO-HDLC) */

static uint32_t crc32_step(uint32_t st, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        st ^= d[i];
        for (int k = 0; k < 8; k++)
            st = (st >> 1) ^ (0xEDB88320u & (0u - (st & 1u)));
    }
    return st;
}

static uint32_t crc32_buf(const uint8_t *d, size_t n)
{
    return crc32_step(0xFFFFFFFFu, d, n) ^ 0xFFFFFFFFu;
}

/* ---------------------------------------------------------------- sniffing */

static bool is_print(uint8_t b)
{
    return (b >= 0x20 && b <= 0x7E) || b == '\r' || b == '\n' || b == '\t';
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c |= 0x20;
    return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

/* NMEA-0183: find "$....*HH" and check the XOR of the bytes between '$' and '*' equals HH. */
static bool nmea_valid(const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (d[i] != '$')
            continue;
        uint8_t xr = 0;
        size_t j = i + 1;
        for (; j < n && d[j] != '*' && d[j] != '\r' && d[j] != '\n'; j++)
            xr ^= d[j];
        if (j + 2 >= n || d[j] != '*')
            continue;
        int hi = hexval(d[j + 1]), lo = hexval(d[j + 2]);
        if (hi >= 0 && lo >= 0 && xr == (uint8_t)(hi * 16 + lo))
            return true;
    }
    return false;
}

/* u-blox UBX: sync B5 62, class, id, len(LE 2), payload, two Fletcher-8 bytes over class..payload. */
static bool ubx_valid(const uint8_t *d, size_t n)
{
    for (size_t i = 0; i + 8 <= n; i++) {
        if (d[i] != 0xB5 || d[i + 1] != 0x62)
            continue;
        uint16_t len = (uint16_t)(d[i + 4] | (d[i + 5] << 8));
        size_t end = i + 6 + len; /* index of ck_a */
        if (end + 1 >= n)
            continue;
        uint8_t a = 0, b = 0;
        for (size_t k = i + 2; k < end; k++) { a += d[k]; b += a; }
        if (a == d[end] && b == d[end + 1])
            return true;
    }
    return false;
}

/*
 * GODIO receiver<->controller frames: 0xF8 command, 0xF9 telemetry; last byte = 8-bit sum of the rest;
 * no length field. A frame is the shortest span from a sync byte whose sum checks and that is followed by
 * another sync or the end of the buffer. Returns the count of valid frames; *sync = the first one's sync
 * byte (0 if none). This also learns the length of message types we have not mapped yet.
 */
static int godio_scan(const uint8_t *d, size_t n, uint8_t *sync)
{
    int count = 0;
    *sync = 0;
    for (size_t i = 0; i + 4 <= n;) {
        if (d[i] != 0xF8 && d[i] != 0xF9) { i++; continue; }
        bool hit = false;
        for (size_t len = 4; len <= 40 && i + len <= n; len++) {
            uint32_t s = 0;
            for (size_t k = 0; k + 1 < len; k++)
                s += d[i + k];
            if ((uint8_t)s == d[i + len - 1] && (i + len == n || d[i + len] == 0xF8 || d[i + len] == 0xF9)) {
                if (!*sync) *sync = d[i];
                count++;
                i += len;
                hit = true;
                break;
            }
        }
        if (!hit) i++;
    }
    return count;
}

static const char *proto_name(uint8_t godio, bool nmea, bool ubx)
{
    return godio == 0xF9 ? "GODIO-F9 (telemetry)" : godio == 0xF8 ? "GODIO-F8 (command)"
         : nmea ? "NMEA" : ubx ? "UBX" : "?";
}

/*
 * pullup: a weak pull-up so an OPEN line reads idle-high instead of noise (sweeping an unknown connector).
 * For a parallel tap across a live, driven line (receiver <-> autopilot controller) pass false: be a pure
 * high-impedance listener and add nothing to the net. TX is always parked off-header, so the line is never
 * driven either way.
 */
static void sniff_config(int pin, uint32_t baud, bool invert, bool pullup)
{
    uart_set_pin(SNIFF_UART, TX_PARK_GPIO, pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_set_baudrate(SNIFF_UART, baud);
    uart_set_line_inverse(SNIFF_UART, invert ? UART_SIGNAL_RXD_INV : UART_SIGNAL_INV_DISABLE);
    if (pullup)
        gpio_pullup_en(pin);
    else
        gpio_pullup_dis(pin);
    gpio_pulldown_dis(pin);
    uart_flush_input(SNIFF_UART);
}

/* Configure UART1 for (pin, baud, invert) and capture up to CAP bytes over the listen window. */
static void probe(int pin, uint32_t baud, bool invert, probe_t *r)
{
    static uint8_t cap[CAP];
    memset(r, 0, sizeof *r);
    r->pin = pin;
    r->baud = baud;
    r->invert = invert;
    sniff_config(pin, baud, invert, true);   /* sweep: idle-high an open connector */

    size_t got = 0;
    int64_t t0 = esp_timer_get_time();
    while ((esp_timer_get_time() - t0) < PRELISTEN_MS * 1000 && got < CAP) {
        int k = uart_read_bytes(SNIFF_UART, cap + got, CAP - got, pdMS_TO_TICKS(20));
        if (k > 0) got += (size_t)k;
    }
    if (got > 0) { /* line is alive: keep listening to the full window */
        while ((esp_timer_get_time() - t0) < WINDOW_MS * 1000 && got < CAP) {
            int k = uart_read_bytes(SNIFF_UART, cap + got, CAP - got, pdMS_TO_TICKS(40));
            if (k > 0) got += (size_t)k;
        }
    }

    r->bytes = got;
    if (got == 0)
        return;
    size_t pr = 0, s = 0;
    bool sample_done = false;   /* the sample stops at its first line; the count runs over everything */
    for (size_t i = 0; i < got; i++) {
        if (!is_print(cap[i]))
            continue;
        pr++;
        if (sample_done)
            continue;
        if (cap[i] >= 0x20 && cap[i] <= 0x7E && s + 1 < sizeof r->sample)
            r->sample[s++] = (char)cap[i];
        else if (cap[i] == '\n' && s > 8)
            sample_done = true;
    }
    r->sample[s] = 0;
    r->printable_pct = (int)(pr * 100 / got);
    r->nmea = nmea_valid(cap, got);
    r->ubx = ubx_valid(cap, got);
    r->godio_cnt = godio_scan(cap, got, &r->godio);
}

static void print_probe(const probe_t *r)
{
    const char *kind = r->godio ? proto_name(r->godio, false, false)
                     : r->nmea ? "NMEA ok" : r->ubx ? "UBX ok " : r->bytes ? "       " : "silent ";
    say("  RX GPIO%d (%s)  %7lu  %-8s  bytes=%-4u print=%3d%%  %s  %s\r\n",
        r->pin, rx_label(r->pin), (unsigned long)r->baud, r->invert ? "inverted" : "normal",
        (unsigned)r->bytes, r->printable_pct, kind, r->godio ? "" : r->sample);
}

static void say_lock(const probe_t *lock)
{
    say("#LOCK RX=GPIO%d (%s) baud=%lu %s fmt=%s\r\n",
        lock->pin, rx_label(lock->pin), (unsigned long)lock->baud,
        lock->invert ? "inverted" : "normal", proto_name(lock->godio, lock->nmea, lock->ubx));
}

/* Relay the locked line to X1 until the mode changes, with "#LOCK" between sentences every LOCK_EVERY_US. */
static void live_dump(const probe_t *lock)
{
    static uint8_t buf[512];
    uint32_t gen = s_mode_gen;
    say("\r\n");
    say_lock(lock);
    sniff_config(lock->pin, lock->baud, lock->invert, true);
    int64_t last = esp_timer_get_time();
    bool at_line_start = true;   /* "#LOCK" only between sentences, never inside one */
    while (s_mode_gen == gen) {
        int k = uart_read_bytes(SNIFF_UART, buf, sizeof buf, pdMS_TO_TICKS(100));
        if (k > 0 && !s_ota_active) {
            con_write(buf, (size_t)k);
            at_line_start = buf[k - 1] == '\n';
            led_toggle();
        }
        int64_t now = esp_timer_get_time();
        if (now - last >= LOCK_EVERY_US && at_line_start) {
            last = now;
            say_lock(lock);
        }
    }
}

/* One raw dump line: "#RAW t=<ms> pin=GPIOn baud=b n=k  HH HH .. | ascii" (binary reverse traffic too). */
static void dump_raw(int64_t t0, int pin, int baud, const uint8_t *d, int n)
{
    static char line[600];
    unsigned long ms = (unsigned long)((esp_timer_get_time() - t0) / 1000);
    int m = n < 160 ? n : 160;   /* cap one line; a longer burst spans several reads/lines */
    int p = snprintf(line, sizeof line, "#RAW t=%lu pin=GPIO%d baud=%d n=%d  ", ms, pin, baud, n);
    for (int i = 0; i < m && p < (int)sizeof line - 4; i++)
        p += snprintf(line + p, sizeof line - p, "%02X ", d[i]);
    if (p < (int)sizeof line - 3) { line[p++] = '|'; line[p++] = ' '; }
    for (int i = 0; i < m && p < (int)sizeof line - 2; i++) {
        char c = (char)d[i];
        line[p++] = (c >= 0x20 && c <= 0x7E) ? c : '.';
    }
    line[p++] = '\r';
    line[p++] = '\n';
    con_write(line, (size_t)p);
}

/*
 * Listen on one X2 pin and dump everything raw (so host/f9.py can parse it). Listen-only: TX stays
 * parked, the line is never driven.
 *   baud > 0: use it.
 *   baud = 0: sweep the baud list and lock onto the first rate that yields VALID frames (GODIO F8/F9,
 *             NMEA or UBX), so it never sticks on noise at the wrong rate; announces "#FOUND" with the
 *             rate and the direction. Use this on an unknown line to identify it.
 * Returns when the mode changes.
 */
static void run_listen(int pin, int baud)
{
    static uint8_t buf[CAP];
    uint32_t gen = s_mode_gen;
    if (pin != 4 && pin != 5)
        pin = 5;
    say("\r\n#LISTEN pin=GPIO%d (%s) baud=%d%s\r\n", pin, rx_label(pin), baud,
        baud ? "" : " (sweeping to identify the line)");

    uint32_t cur = baud ? (uint32_t)baud : BAUDS[0];
    bool locked = baud != 0;
    size_t bidx = 0;

    /* Phase 1 (sweep only): find the rate that gives valid frames. */
    while (!locked && s_mode_gen == gen) {
        cur = BAUDS[bidx];
        sniff_config(pin, cur, false, false);
        size_t got = 0;
        int64_t t0 = esp_timer_get_time();
        while ((esp_timer_get_time() - t0) < 900 * 1000 && got < CAP && s_mode_gen == gen) {
            int k = uart_read_bytes(SNIFF_UART, buf + got, CAP - got, pdMS_TO_TICKS(50));
            if (k > 0) got += (size_t)k;
        }
        uint8_t sync = 0;
        bool g = godio_scan(buf, got, &sync) > 0, nm = nmea_valid(buf, got), ux = ubx_valid(buf, got);
        if (g || nm || ux) {
            say("#FOUND pin=GPIO%d (%s) baud=%lu %s\r\n", pin, rx_label(pin), (unsigned long)cur,
                proto_name(sync, nm, ux));
            locked = true;
        } else {
            say("  GPIO%d %lu: %u bytes, no valid frame\r\n", pin, (unsigned long)cur, (unsigned)got);
            bidx = (bidx + 1) % (sizeof BAUDS / sizeof BAUDS[0]);
        }
    }

    /* Phase 2: raw dump at the chosen rate, with a "#FOUND" reminder every few seconds. */
    sniff_config(pin, cur, false, false);
    int64_t t0 = esp_timer_get_time(), last_note = t0, last_quiet = t0;
    while (s_mode_gen == gen) {
        int k = uart_read_bytes(SNIFF_UART, buf, 256, pdMS_TO_TICKS(50));
        int64_t now = esp_timer_get_time();
        if (k > 0) {
            if (!s_ota_active) {
                dump_raw(t0, pin, (int)cur, buf, k);
                led_toggle();
            }
            last_quiet = now;
            if (now - last_note > 3000000 && !s_ota_active) {
                uint8_t sync = 0;
                godio_scan(buf, (size_t)k, &sync);
                if (sync)
                    say("#FOUND pin=GPIO%d baud=%lu %s\r\n", pin, (unsigned long)cur,
                        sync == 0xF9 ? "GODIO-F9 (telemetry)" : "GODIO-F8 (command)");
                last_note = now;
            }
        } else if (now - last_quiet > 3000000 && !s_ota_active) {
            say("#QUIET pin=GPIO%d baud=%lu\r\n", pin, (unsigned long)cur);
            last_quiet = now;
        }
    }
}

/* ---------------------------------------------------------------- update over X1 */

static bool con_read_exact(uint8_t *d, size_t n, int timeout_ms)
{
    size_t got = 0;
    int64_t until = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (got < n) {
        int64_t left = (until - esp_timer_get_time()) / 1000;
        if (left <= 0)
            return false;
        int k = uart_read_bytes(CON_UART, d + got, n - got, pdMS_TO_TICKS(left < 50 ? left : 50));
        if (k > 0) got += (size_t)k;
    }
    return true;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/*
 * One update: "#KOGG-OTA <size> <crc32>" was received. Frames from the host:
 *   A5 5A | len u16 LE | offset u32 LE | data[len] | crc32(data) u32 LE
 * Replies (lines): "#OTA READY <slot> <slot size>", "#OTA ACK <bytes held>", "#OTA NAK <bytes held>",
 * "#OTA DONE ...", "#OTA FAIL <reason>". The host always continues from the number in ACK/NAK, so a
 * lost reply or a repeated chunk cannot shift the image. The whole image is checked with the CRC from the
 * header and by esp_ota_end (image header and SHA-256) before the boot slot is switched.
 */
static void ota_session(const char *args)
{
    static uint8_t data[OTA_CHUNK_MAX];
    unsigned long size = 0, want_crc = 0;
    if (sscanf(args, "%lu %lx", &size, &want_crc) != 2 || size == 0) {
        ota_say("\r\n#OTA FAIL bad-header\r\n");
        return;
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        ota_say("\r\n#OTA FAIL no-slot\r\n");
        return;
    }
    if (size > part->size) {
        ota_say("\r\n#OTA FAIL too-big %lu > %lu\r\n", size, (unsigned long)part->size);
        return;
    }
    s_ota_active = true;
    vTaskDelay(pdMS_TO_TICKS(50));   /* a relay write already past the gate finishes first */
    esp_ota_handle_t h;
    esp_err_t e = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (e != ESP_OK) {
        ota_say("\r\n#OTA FAIL begin %s\r\n", esp_err_to_name(e));
        s_ota_active = false;
        return;
    }
    ota_say("\r\n#OTA READY %s %lu\r\n", part->label, (unsigned long)part->size);

    uint32_t held = 0, run = 0xFFFFFFFFu;
    const char *fail = NULL;
    while (held < size && !fail) {
        /* sync A5 5A, then the rest of the header */
        uint8_t c, prev = 0, hdr[6], tail[4];
        bool synced = false;
        int64_t until = esp_timer_get_time() + (int64_t)OTA_IDLE_MS * 1000;
        while (!synced && esp_timer_get_time() < until) {
            if (uart_read_bytes(CON_UART, &c, 1, pdMS_TO_TICKS(50)) == 1) {
                synced = prev == 0xA5 && c == 0x5A;
                prev = c;
            }
        }
        if (!synced) { fail = "timeout"; break; }
        if (!con_read_exact(hdr, sizeof hdr, 1000)) { ota_say("\r\n#OTA NAK %lu\r\n", (unsigned long)held); continue; }
        uint16_t len = (uint16_t)(hdr[0] | hdr[1] << 8);
        uint32_t off = le32(hdr + 2);
        if (len == 0 || len > OTA_CHUNK_MAX
            || !con_read_exact(data, len, 2000) || !con_read_exact(tail, sizeof tail, 1000)
            || crc32_buf(data, len) != le32(tail)) {
            ota_say("\r\n#OTA NAK %lu\r\n", (unsigned long)held);
            continue;
        }
        if (off != held) {   /* a repeat (our ACK was lost) or a gap: tell where we are */
            ota_say("\r\n#OTA %s %lu\r\n", off < held ? "ACK" : "NAK", (unsigned long)held);
            continue;
        }
        if (held + len > size) { fail = "overrun"; break; }
        if (esp_ota_write(h, data, len) != ESP_OK) { fail = "flash-write"; break; }
        run = crc32_step(run, data, len);
        held += len;
        led_toggle();
        ota_say("\r\n#OTA ACK %lu\r\n", (unsigned long)held);
    }
    if (!fail && (run ^ 0xFFFFFFFFu) != (uint32_t)want_crc)
        fail = "image-crc";
    if (fail) {
        esp_ota_abort(h);
        ota_say("\r\n#OTA FAIL %s at %lu\r\n", fail, (unsigned long)held);
        s_ota_active = false;
        return;
    }
    e = esp_ota_end(h);   /* frees the handle whatever it returns */
    if (e == ESP_OK)
        e = esp_ota_set_boot_partition(part);
    if (e != ESP_OK) {
        ota_say("\r\n#OTA FAIL %s\r\n", esp_err_to_name(e));
        s_ota_active = false;
        return;
    }
    ota_say("\r\n#OTA DONE %s: rebooting into it\r\n", part->label);
    uart_wait_tx_done(CON_UART, pdMS_TO_TICKS(500));
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

static void request_mode(mode_t m)
{
    s_mode = m;
    s_mode_gen++;   /* makes the running sweep/live_dump/listen loop return and re-dispatch */
}

/* Watches X1 for one-line commands; anything else arriving on X1 is ignored. */
static void con_task(void *arg)
{
    (void)arg;
    static char line[96];
    size_t ln = 0;
    uint8_t b;
    for (;;) {
        if (uart_read_bytes(CON_UART, &b, 1, portMAX_DELAY) != 1)
            continue;
        if (b == '\n' || b == '\r') {
            line[ln] = 0;
            if (strncmp(line, "#KOGG-OTA ", 10) == 0) {
                ota_session(line + 10);
            } else if (strncmp(line, "#LISTEN", 7) == 0) {
                int pin = 5, baud = 115200;
                sscanf(line + 7, "%d %d", &pin, &baud);
                s_listen_pin = pin;
                s_listen_baud = baud;
                request_mode(MODE_LISTEN);
            } else if (strncmp(line, "#SNIFF", 6) == 0) {
                request_mode(MODE_SWEEP);
            }
            ln = 0;
        } else if (ln + 1 < sizeof line) {
            line[ln++] = (char)b;
        } else {
            ln = 0;
        }
    }
}

static const char *ota_state_name(esp_ota_img_states_t st)
{
    switch (st) {
    case ESP_OTA_IMG_NEW:            return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending";
    case ESP_OTA_IMG_VALID:          return "valid";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "aborted";
    default:                         return "undefined";
    }
}

/* A freshly updated image that ran CONFIRM_AFTER_US keeps its slot; one that resets before that is
 * rolled back by the bootloader to the previous image. */
static void confirm_cb(void *arg)
{
    (void)arg;
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
        say("\r\n#OTA image %s confirmed\r\n", esp_ota_get_running_partition()->label);
}

/* ---------------------------------------------------------------- main */

void app_main(void)
{
    const gpio_config_t led = {.pin_bit_mask = 1ull << LED_GPIO, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&led);
    led_set(1);                       /* LED on = powered and running */

    const uart_config_t con = {
        .baud_rate = CON_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    /* No driver TX ring: writes sleep on the FIFO instead of spinning on a full ring (uline.c). */
    ESP_ERROR_CHECK(uart_driver_install(CON_UART, 2 * OTA_CHUNK_MAX, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(CON_UART, &con));
    ESP_ERROR_CHECK(uart_set_pin(CON_UART, CON_TX_GPIO, CON_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    xTaskCreate(con_task, "con", 4096, NULL, 10, NULL);

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(running, &st);
    say("\r\n=== Kogger GPS sniffer %s (listen-only, build %s %s) ===\r\n", SNIFFER_VERSION, __DATE__, __TIME__);
    say("#BOOT slot=%s state=%s\r\n", running ? running->label : "?", ota_state_name(st));
    if (st == ESP_OTA_IMG_PENDING_VERIFY) {
        const esp_timer_create_args_t ta = {.callback = confirm_cb, .name = "confirm"};
        esp_timer_handle_t t;
        if (esp_timer_create(&ta, &t) == ESP_OK)
            esp_timer_start_once(t, CONFIRM_AFTER_US);
    }
    say("Wire: GPS GND -> X2 pin3 (first). GPS signal pins -> X2 pin1 (GPIO5) and pin2 (GPIO4). GPS VCC: not connected.\r\n");

    const uart_config_t sn = {
        .baud_rate = (int)BAUDS[0],
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(SNIFF_UART, RX_BUF, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(SNIFF_UART, &sn));

    for (;;) {
        if (s_mode == MODE_LISTEN) {
            run_listen(s_listen_pin, s_listen_baud);
            continue;
        }
        uint32_t gen = s_mode_gen;
        say("Sweeping RX pin x baud x inversion, scoring for NMEA/UBX...\r\n");
        probe_t best;
        memset(&best, 0, sizeof best);
        bool locked = false, any_bytes = false;
        /* Non-inverted first (the normal case); inverted only if nothing locked. */
        for (int inv = 0; inv <= 1 && !locked && s_mode_gen == gen; inv++) {
            if (inv)
                say("-- no valid frames yet: retrying inverted --\r\n");
            for (size_t pi = 0; pi < sizeof RX_PINS / sizeof RX_PINS[0] && !locked && s_mode_gen == gen; pi++) {
                for (size_t bi = 0; bi < sizeof BAUDS / sizeof BAUDS[0] && s_mode_gen == gen; bi++) {
                    probe_t r;
                    probe(RX_PINS[pi], BAUDS[bi], inv, &r);
                    print_probe(&r);
                    if (r.bytes)
                        any_bytes = true;
                    if (r.nmea || r.ubx || r.godio) { best = r; locked = true; break; }
                    if (r.bytes && r.printable_pct > best.printable_pct) best = r;
                }
            }
        }
        if (s_mode_gen != gen)
            continue;               /* a command switched the mode mid-sweep */
        if (locked) {
            live_dump(&best);       /* returns only on a mode change */
            continue;
        }
        if (!any_bytes) {
            say("#NOLOCK no bytes on either X2 pin at any baud: boat powered? GND common? signal pins on X2 pin1/pin2?\r\n");
        } else {
            say("#NOLOCK bytes but no valid frame; best readable RX=GPIO%d (%s) %lu %s print=%d%% \"%s\" - "
                "maybe a non-NMEA protocol or an unlisted baud\r\n",
                best.pin, rx_label(best.pin), (unsigned long)best.baud, best.invert ? "inverted" : "normal",
                best.printable_pct, best.sample);
        }
        for (int i = 0; i < RESWEEP_MS / 100 && s_mode_gen == gen; i++)
            vTaskDelay(pdMS_TO_TICKS(100));
    }
}
