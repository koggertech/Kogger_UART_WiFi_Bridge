#include "netcfg.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"

static const char *TAG = "netcfg";

static wifi_role_t s_role = ROLE_STA, s_saved_role = ROLE_STA;
static ap_cfg_t s_ap;
static ip_cfg_t s_ip = { .ip = { 10, 0, 0, 10 }, .mask = { 255, 255, 255, 0 }, .dhcp = 1,
                         .pool_start = { 10, 0, 0, 11 }, .pool_end = { 10, 0, 0, 30 }, .lease_min = 120, .offer = 0 };
/* Line 1 pins: connector X2 of the module board (schematic 2026-09-26: GPIO5 -> R7 -> pin 1 TX,
 * pin 2 -> R11 -> GPIO4 RX). Both lines start switched off in the station role; the access point role
 * turns them on below. The rates here are defaults only: the running rates belong to ports.c. */
static line_cfg_t s_line[NLINES] = {
    { .mode = LINE_OFF, .dest = DEST_SENDERS, .rport = 14444, .lport = 14444, .baud = 921600, .tx_pin = 21, .rx_pin = 20 },
    { .mode = LINE_OFF, .dest = DEST_SENDERS, .rport = 14445, .lport = 14445, .baud = 115200, .tx_pin = 5, .rx_pin = 4 },
};
static uint8_t s_addr = 87;

/* Layout of the 0.7 "relay" record, migrated into line 0 on the first boot of this firmware. */
typedef struct {
    uint8_t  mode, addr, ip[4];
    uint16_t rport, lport;
} relay07_t;

static uint32_t u32ip(const uint8_t *a)
{
    return (uint32_t)a[0] << 24 | (uint32_t)a[1] << 16 | (uint32_t)a[2] << 8 | a[3]; /* host order for maths */
}

static bool line_ok_self(int line, const line_cfg_t *c);

static bool put_blob(const char *key, const void *v, size_t n)
{
    nvs_handle_t h;
    if (nvs_open("wb", NVS_READWRITE, &h) != ESP_OK)
        return false;
    bool ok = nvs_set_blob(h, key, v, n) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (!ok)
        ESP_LOGW(TAG, "%s not saved", key);
    return ok;
}

static bool get_blob(nvs_handle_t h, const char *key, void *v, size_t n)
{
    size_t sz = n;
    return nvs_get_blob(h, key, v, &sz) == ESP_OK && sz == n;
}

void netcfg_ap_default(ap_cfg_t *c)
{
    uint8_t mac[6];
    memset(c, 0, sizeof *c);
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(c->ssid, sizeof c->ssid, "Kogger-%02X%02X", mac[4], mac[5]);
    strcpy(c->pass, "kogger1234"); /* documented default; to be changed on first setup */
    c->channel = 6;
    c->max_clients = 4;
    c->auth = 1;
}

void netcfg_load(void)
{
    netcfg_ap_default(&s_ap);

    nvs_handle_t h;
    if (nvs_open("wb", NVS_READWRITE, &h) != ESP_OK)
        return;
    uint8_t v;
    if (nvs_get_u8(h, "role", &v) == ESP_OK && v <= ROLE_AP)
        s_role = (wifi_role_t)v;
    s_saved_role = s_role;
    if (s_role == ROLE_AP) {
        /* A bridge with nothing saved yet: both connectors on their UDP ports, answering whoever talks
         * to them, so the module is reachable over the network at once. Its own address differs from a
         * station module's (87), so a pair of modules on one link can be told apart. */
        s_line[0].mode = LINE_UDP;
        s_line[1].mode = LINE_UDP;
        s_addr = 88;
    }
    ap_cfg_t ap;
    if (get_blob(h, "ap", &ap, sizeof ap)) {
        if (ap.channel > 11 && ap.channel <= 13)
            ap.channel = 11; /* saved by 0.10, which let 12/13 through: nearest channel the driver takes */
        if (netcfg_ap_ok(&ap))
            s_ap = ap;
    }
    ip_cfg_t ip;
    if (get_blob(h, "ipcfg", &ip, sizeof ip) && netcfg_ip_ok(&ip))
        s_ip = ip;
    if (nvs_get_u8(h, "maddr", &v) == ESP_OK && v != 0 && v != 255)
        s_addr = v; /* 0.11 let 255 through; it is the broadcast route now: the role's default instead */
    for (int i = 0; i < NLINES; i++) {
        line_cfg_t l;
        char key[8];
        snprintf(key, sizeof key, "line%d", i);
        size_t sz = 0;
        bool saved = nvs_get_blob(h, key, NULL, &sz) != ESP_ERR_NVS_NOT_FOUND;
        if (get_blob(h, key, &l, sizeof l) && line_ok_self(i, &l)) {
            s_line[i] = l;
        } else if (saved) {
            /* another layout (a later version) or damaged: the default runs, and the 0.7 record kept
             * for rollbacks must never come back over it */
            ESP_LOGW(TAG, "%s unreadable: default kept", key);
        } else if (i == 0) {
            relay07_t r;
            /* a switched-off 0.7 relay carries nothing the defaults do not: the role's default stays */
            if (get_blob(h, "relay", &r, sizeof r) && r.mode != LINE_OFF && r.mode <= LINE_TCP) {
                s_line[0].mode = r.mode;
                memcpy(s_line[0].ip, r.ip, 4);
                s_line[0].dest = u32ip(r.ip) == 0 ? DEST_SENDERS : DEST_FIXED;
                s_line[0].rport = r.rport;
                s_line[0].lport = r.lport;
                if (r.addr != 0 && r.addr != 255)
                    s_addr = r.addr;
                nvs_set_blob(h, "line0", &s_line[0], sizeof s_line[0]);
                nvs_set_u8(h, "maddr", s_addr);
                nvs_commit(h); /* the old record stays: a rollback to 0.7 still finds it */
                ESP_LOGI(TAG, "0.7 relay settings moved to line 0");
            }
        }
    }
    /* X2's saved rate is a port setting: kept apart from the line record, so "forget lines" keeps it */
    uint32_t b1;
    if (nvs_get_u32(h, "baud1", &b1) == ESP_OK && b1 >= 9600 && b1 <= 4000000)
        s_line[1].baud = b1;
    nvs_close(h);
    ESP_LOGI(TAG, "role %s, AP \"%s\" ch %u, %u.%u.%u.%u, line0 mode %u, line1 mode %u pins %d/%d",
             s_role == ROLE_AP ? "AP" : "station", s_ap.ssid, s_ap.channel, s_ip.ip[0], s_ip.ip[1], s_ip.ip[2],
             s_ip.ip[3], s_line[0].mode, s_line[1].mode, s_line[1].tx_pin, s_line[1].rx_pin);
}

/* ---- role --------------------------------------------------------------------------------------- */

wifi_role_t netcfg_role(void)
{
    return s_role;
}

wifi_role_t netcfg_saved_role(void)
{
    return s_saved_role;
}

bool netcfg_set_role(wifi_role_t r)
{
    if (r > ROLE_AP)
        return false;
    nvs_handle_t h;
    if (nvs_open("wb", NVS_READWRITE, &h) != ESP_OK)
        return false;
    bool ok = nvs_set_u8(h, "role", (uint8_t)r) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok)
        s_saved_role = r; /* s_role keeps describing the running firmware until the reboot */
    return ok;
}

bool netcfg_forget_lines(void)
{
    nvs_handle_t h;
    if (nvs_open("wb", NVS_READWRITE, &h) != ESP_OK)
        return false;
    static const char *const KEYS[] = { "line0", "line1", "maddr", "relay" }; /* "relay" would migrate back */
    for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++)
        nvs_erase_key(h, KEYS[i]);
    bool ok = nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

/* ---- access point ------------------------------------------------------------------------------- */

void netcfg_ap(ap_cfg_t *c)
{
    *c = s_ap;
}

bool netcfg_ap_ok(const ap_cfg_t *c)
{
    size_t sl = strnlen(c->ssid, sizeof c->ssid), pl = strnlen(c->pass, sizeof c->pass);
    if (sl < 1 || sl > 32 || pl > 63 || c->auth > 2 || c->hidden > 1)
        return false;
    if (c->auth != 0 && pl < 8)
        return false; /* WPA2/WPA3 need 8..63 characters */
    /* 1..11: the driver's default country ("01") allows no more, and an AP on channel 12/13 is
     * refused by esp_wifi_set_config (IDF docs, wifi.rst "Wi-Fi Country Code"). */
    return c->channel >= 1 && c->channel <= 11 && c->max_clients >= 1 && c->max_clients <= 10;
}

/* Setters change the running copy only once NVS has taken the value: what is read back is what runs. */
bool netcfg_set_ap(const ap_cfg_t *c)
{
    if (!netcfg_ap_ok(c) || !put_blob("ap", c, sizeof *c))
        return false;
    s_ap = *c;
    return true;
}

/* ---- IP / DHCP ------------------------------------------------------------------------------------- */

void netcfg_ip(ip_cfg_t *c)
{
    *c = s_ip;
}

bool netcfg_ip_ok(const ip_cfg_t *c)
{
    uint32_t ip = u32ip(c->ip), mask = u32ip(c->mask);
    if (mask == 0 || mask == 0xFFFFFFFFu || (~mask & (~mask + 1)) != 0)
        return false; /* contiguous mask, not /0 or /32 */
    uint32_t net = ip & mask, bcast = net | ~mask;
    if (ip == net || ip == bcast)
        return false;
    if (c->lease_min < 1 || c->lease_min > 2880 || c->offer > (IPCFG_OFFER_GW | IPCFG_OFFER_DNS) || c->dhcp > 1)
        return false;
    if (!c->dhcp)
        return true;
    uint32_t a = u32ip(c->pool_start), b = u32ip(c->pool_end);
    if ((a & mask) != net || (b & mask) != net || a >= b || a == net || b == bcast)
        return false; /* the IDF DHCP server also refuses a pool of one address */
    if (ip >= a && ip <= b)
        return false; /* the AP's own address is not handed out */
    return b - a + 1 <= 100; /* DHCPS_MAX_LEASE of the IDF DHCP server */
}

bool netcfg_set_ip(const ip_cfg_t *c)
{
    if (!netcfg_ip_ok(c) || !put_blob("ipcfg", c, sizeof *c))
        return false;
    s_ip = *c;
    return true;
}

/* ---- lines ------------------------------------------------------------------------------------------ */

bool netcfg_pin_ok(int pin)
{
    /* ESP32-C3-MINI-1U: GPIO2/8/9 strap the boot mode, 11..17 belong to the flash, 18/19 are USB,
     * 20/21 are UART0 (line 0). */
    return pin == 0 || pin == 1 || pin == 3 || pin == 4 || pin == 5 || pin == 6 || pin == 7 || pin == 10;
}

void netcfg_line(int line, line_cfg_t *c)
{
    *c = s_line[line];
}

static uint16_t eff_lport(const line_cfg_t *c)
{
    return c->lport ? c->lport : c->rport;
}

static bool line_ok_self(int line, const line_cfg_t *c)
{
    if (line < 0 || line >= NLINES || c->mode > LINE_TCP || c->dest > DEST_BROADCAST)
        return false;
    if (c->mode == LINE_UDP) {
        if (eff_lport(c) == 0)
            return false;
        if (c->dest == DEST_FIXED && (u32ip(c->ip) == 0 || c->rport == 0))
            return false;
        if (c->dest == DEST_BROADCAST && c->rport == 0)
            return false;
    } else if (c->mode == LINE_TCP) {
        if (u32ip(c->ip) == 0 || c->rport == 0)
            return false; /* a TCP client needs somebody to call */
    }
    if (line == 1) {
        if (c->baud < 9600 || c->baud > 4000000)
            return false;
        bool tx = c->tx_pin >= 0, rx = c->rx_pin >= 0;
        if ((tx && !netcfg_pin_ok(c->tx_pin)) || (rx && !netcfg_pin_ok(c->rx_pin)) || (tx && rx && c->tx_pin == c->rx_pin))
            return false;
    }
    return true;
}

bool netcfg_line_ok(int line, const line_cfg_t *c)
{
    if (!line_ok_self(line, c))
        return false;
    /* two UDP lines cannot listen on the same port */
    const line_cfg_t *o = &s_line[line ^ 1];
    return !(c->mode == LINE_UDP && o->mode == LINE_UDP && eff_lport(c) == eff_lport(o));
}

bool netcfg_set_line(int line, const line_cfg_t *c)
{
    if (!netcfg_line_ok(line, c))
        return false;
    char key[8];
    snprintf(key, sizeof key, "line%d", line);
    if (!put_blob(key, c, sizeof *c))
        return false;
    s_line[line] = *c;
    return true;
}

/* The rate goes into "baud1" and, when a line 1 record exists, into it too (0.11 reads only the record). */
static bool put_baud1(uint32_t baud, bool erase)
{
    nvs_handle_t h;
    if (nvs_open("wb", NVS_READWRITE, &h) != ESP_OK)
        return false;
    bool ok = (erase ? nvs_erase_key(h, "baud1") != ESP_FAIL : nvs_set_u32(h, "baud1", baud) == ESP_OK);
    line_cfg_t l;
    if (ok && get_blob(h, "line1", &l, sizeof l)) {
        l.baud = baud;
        ok = nvs_set_blob(h, "line1", &l, sizeof l) == ESP_OK;
    }
    ok = ok && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok)
        s_line[1].baud = baud;
    return ok;
}

bool netcfg_set_baud1(uint32_t baud)
{
    return baud >= 9600 && baud <= 4000000 && put_baud1(baud, false);
}

bool netcfg_erase_baud1(void)
{
    return put_baud1(115200, true);
}

uint8_t netcfg_addr(void)
{
    return s_addr;
}

bool netcfg_set_addr(uint8_t a)
{
    if (a == 0 || a == 255)
        return false; /* 0 is where devices sit by default, 255 is the broadcast route */
    nvs_handle_t h;
    if (nvs_open("wb", NVS_READWRITE, &h) != ESP_OK)
        return false;
    bool ok = nvs_set_u8(h, "maddr", a) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok)
        s_addr = a;
    return ok;
}
