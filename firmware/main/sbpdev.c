#include "sbpdev.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "link.h"
#include "manager.h"
#include "netcfg.h"
#include "ota.h"
#include "relay.h"
#include "uline.h"

static const char *TAG = "sbpdev";

#define REPORT_MS_DEFAULT 1000

static volatile uint8_t s_route;      /* current bus address (frame byte 2) */
static uint8_t s_def_route;           /* address applied at boot */
static uint8_t s_mark;                /* set by ID_MARK, cleared by reboot: tells the host we kept its settings */
static uint16_t s_report_ms = REPORT_MS_DEFAULT;
static sbp_chan_t s_chan;             /* manager task only: where answers and reports go */
static uint32_t s_boot_baud;          /* rate for sbpdev_start(): one-shot resume, else saved; 0 none */
static bool s_route_resumed;          /* this boot's address came from the one-shot resume record */

/* ---- settings in NVS (saved only by ID_FLASH v0, like the sonars) --------------------------- */

static void settings_load(bool with_baud)
{
    nvs_handle_t h;
    if (nvs_open("wb", NVS_READONLY, &h) != ESP_OK)
        return;
    uint8_t r;
    uint16_t ms;
    uint32_t baud;
    if (nvs_get_u8(h, "route", &r) == ESP_OK)
        s_def_route = r;
    if (nvs_get_u16(h, "rep_ms", &ms) == ESP_OK)
        s_report_ms = ms;
    if (with_baud && nvs_get_u32(h, "baud", &baud) == ESP_OK)
        s_boot_baud = baud;
    nvs_close(h);
}

static bool settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("wb", NVS_READWRITE, &h) != ESP_OK)
        return false;
    bool ok = nvs_set_u8(h, "route", s_def_route) == ESP_OK && nvs_set_u16(h, "rep_ms", s_report_ms) == ESP_OK &&
              nvs_set_u32(h, "baud", link_baud()) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

static bool settings_erase(void)
{
    nvs_handle_t h;
    if (nvs_open("wb", NVS_READWRITE, &h) != ESP_OK)
        return false;
    nvs_erase_key(h, "route");
    nvs_erase_key(h, "rep_ms");
    nvs_erase_key(h, "baud");
    bool ok = nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

void sbpdev_save_resume(void)
{
    nvs_handle_t h;
    if (nvs_open("wb", NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_set_u32(h, "rtbaud", link_baud());
    nvs_set_u8(h, "rtroute", s_route);
    nvs_commit(h);
    nvs_close(h);
}

/* One-shot values written just before an update reboot win over the saved ones, then vanish. */
static void resume_load(void)
{
    nvs_handle_t h;
    if (nvs_open("wb", NVS_READWRITE, &h) != ESP_OK)
        return;
    uint32_t baud;
    uint8_t r;
    if (nvs_get_u32(h, "rtbaud", &baud) == ESP_OK) {
        s_boot_baud = baud;
        nvs_erase_key(h, "rtbaud");
    }
    if (nvs_get_u8(h, "rtroute", &r) == ESP_OK) {
        s_route = r;
        s_route_resumed = true; /* relay_start() leaves it alone even while a line bridges */
        nvs_erase_key(h, "rtroute");
    }
    nvs_commit(h);
    nvs_close(h);
}

uint8_t sbpdev_route(void)
{
    return s_route;
}

uint8_t sbpdev_default_route(void)
{
    return s_def_route;
}

void sbpdev_set_route(uint8_t r)
{
    s_route = r;
}

void sbpdev_clear_mark(void)
{
    s_mark = 0;
}

void sbpdev_load(void)
{
    settings_load(true);
    s_route = s_def_route;
    resume_load();
    /* A station module sits on the host's port, as before 0.10. An access point may have a device on
     * that port instead: it hears from the module only after somebody asked the module something. */
    s_chan.kind = netcfg_role() == ROLE_AP ? CH_NONE : CH_LINK;
}

void sbpdev_start(void)
{
    if (s_boot_baud && s_boot_baud != link_baud())
        link_set_baud(s_boot_baud);
    ESP_LOGI(TAG, "SBP address %u, report %u ms, baud %lu", s_route, s_report_ms, (unsigned long)link_baud());
}

bool sbpdev_route_resumed(void)
{
    return s_route_resumed;
}

/* ---- channels and framing ------------------------------------------------------------------- */

void sbpdev_set_channel(const sbp_chan_t *ch)
{
    s_chan = *ch;
}

void sbpdev_channel(sbp_chan_t *ch)
{
    *ch = s_chan;
}

bool sbpdev_has_listener(void)
{
    return s_chan.kind == CH_LINK ? link_proto() == LINK_PROTO_SBP : s_chan.kind != CH_NONE;
}

/* Every frame of the module leaves here, from the manager task only. */
static void emit(uint8_t mode, uint8_t id, const uint8_t *payload, uint8_t len)
{
    uint8_t f[SBP_FRAME_MAX];
    switch (s_chan.kind) {
    case CH_LINK:
        link_send_sbp(s_route, mode, id, payload, len);
        break;
    case CH_LINE1:
        uline_tx_frame(f, sbp_encode(s_route, mode, id, payload, len, f));
        break;
    case CH_NET:
        relay_send_to(s_chan.line, s_chan.ip, s_chan.port, f, sbp_encode(s_route, mode, id, payload, len, f));
        break;
    default:
        break; /* nobody has asked the module anything yet */
    }
}

void sbpdev_send(uint8_t type, uint8_t ver, uint8_t id, const uint8_t *payload, uint8_t len)
{
    emit(SBP_MODE(type, ver, 0, s_mark), id, payload, len);
}

void sbpdev_ack(const sbp_frame_t *req, uint8_t code)
{
    const uint8_t pl[3] = { code, req->ck1, req->ck2 };
    emit(SBP_MODE(SBP_T_CONTENT, sbp_ver(req->mode), 1, s_mark), req->id, pl, sizeof pl);
}

uint16_t sbpdev_report_ms(void)
{
    return s_report_ms;
}

void sbpdev_set_report_ms(uint16_t ms)
{
    s_report_ms = ms;
}

/* ---- RX side (link RX task) ----------------------------------------------------------------- */

void sbpdev_on_frame_ch(const sbp_frame_t *f, const sbp_chan_t *ch)
{
    uint8_t t = sbp_type(f->mode);
    if (f->route != s_route || (t != SBP_T_SETTING && t != SBP_T_GETTING))
        return; /* another device's frame, or a CONTENT echo */
    if (ch->kind == CH_LINK)
        link_set_proto(LINK_PROTO_SBP);
    manager_post_sbp(f, ch);
}

void sbpdev_on_frame(const sbp_frame_t *f)
{
    const sbp_chan_t ch = { .kind = CH_LINK };
    sbpdev_on_frame_ch(f, &ch);
}

/* ---- identity ------------------------------------------------------------------------------- */

static void fw_version(uint8_t *major, uint8_t *minor)
{
    unsigned ma = 0, mi = 0;
    sscanf(esp_app_get_description()->version, "%u.%u", &ma, &mi);
    *major = (uint8_t)ma;
    *minor = (uint8_t)mi;
}

static void send_version(uint8_t ver)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (ver == 0) {
        /* boardMinor, board, 3x U2, U4, U2, serial U4 at offset 14; 34 bytes like the sonars */
        uint8_t p[34] = { 0 };
        p[1] = SBP_BOARD_WIFI;
        sbp_put_u32(&p[14], (uint32_t)mac[2] << 24 | (uint32_t)mac[3] << 16 | (uint32_t)mac[4] << 8 | mac[5]);
        sbpdev_send(SBP_T_CONTENT, 0, SBP_ID_VERSION, p, sizeof p);
    } else if (ver == 1) {
        uint8_t p[12] = { 0 };
        memcpy(p, mac, 6);
        sbpdev_send(SBP_T_CONTENT, 1, SBP_ID_VERSION, p, sizeof p);
    } else if (ver == 2) {
        /* bootMode, boardMinor, board, bootMinor, bootMajor, U2, fwMinor, fwMajor */
        uint8_t p[9] = { 0 };
        p[0] = ota_boot_mode();       /* 1 while an update window/transfer is open: KoggerApp waits for it */
        p[2] = SBP_BOARD_WIFI;
        fw_version(&p[8], &p[7]);
        sbpdev_send(SBP_T_CONTENT, 2, SBP_ID_VERSION, p, sizeof p);
    }
}

/* ---- ID_UART (KoggerApp IDBinUART) ---------------------------------------------------------- */

/* The rate of ID_UART v0 is the one of the UART behind the asking channel: line 1 when the frame came
 * over UART1 or to line 1's socket, otherwise line 0 (the host link). */
static int chan_line(void)
{
    return s_chan.kind == CH_LINE1 || (s_chan.kind == CH_NET && s_chan.line == 1) ? 1 : 0;
}

static uint32_t chan_baud(void)
{
    if (chan_line() == 0)
        return link_baud();
    line_cfg_t c;
    netcfg_line(1, &c);
    return c.baud;
}

static void send_uart(uint8_t ver)
{
    uint8_t p[9];
    sbp_put_u32(p, SBP_KEY_CONFIRM);
    if (ver == 0) {
        p[4] = 1;
        sbp_put_u32(&p[5], chan_baud());
        sbpdev_send(SBP_T_CONTENT, 0, SBP_ID_UART, p, 9);
    } else if (ver == 1) {
        p[4] = 1;
        p[5] = s_route;
        sbpdev_send(SBP_T_CONTENT, 1, SBP_ID_UART, p, 6);
    } else if (ver == 2) {
        p[4] = s_def_route;
        sbpdev_send(SBP_T_CONTENT, 2, SBP_ID_UART, p, 5);
    }
}

static void handle_uart(const sbp_frame_t *f)
{
    uint8_t ver = sbp_ver(f->mode);
    if (ver > 2) {
        sbpdev_ack(f, SBP_RESP_ERR_VERSION);
        return;
    }
    if (sbp_type(f->mode) == SBP_T_GETTING) {
        send_uart(ver);
        return;
    }
    if (f->len < 5 || sbp_get_u32(f->payload) != SBP_KEY_CONFIRM) {
        sbpdev_ack(f, SBP_RESP_ERR_KEY);
        return;
    }
    if (ver == 0) {                                 /* key, uart id, baud */
        uint32_t baud = f->len >= 9 ? sbp_get_u32(f->payload + 5) : 0;
        if (baud < LINK_BAUD_MIN || baud > LINK_BAUD_MAX) {
            sbpdev_ack(f, SBP_RESP_ERR_PAYLOAD);
            return;
        }
        if (chan_line() == 1) {                     /* line 1 settings are always saved at once */
            line_cfg_t c;
            netcfg_line(1, &c);
            c.baud = baud;
            if (!netcfg_line_ok(1, &c)) {
                sbpdev_ack(f, SBP_RESP_ERR_PAYLOAD);
                return;
            }
            sbpdev_ack(f, SBP_RESP_OK);             /* leaves at the old rate: queued before the switch */
            relay_line_set(1, &c);
            return;
        }
        sbpdev_ack(f, SBP_RESP_OK);                 /* leaves at the old rate: queued before the switch */
        link_set_baud(baud);
    } else if (ver == 1) {                          /* key, uart id, address */
        if (f->len < 6) {
            sbpdev_ack(f, SBP_RESP_ERR_PAYLOAD);
            return;
        }
        sbpdev_ack(f, SBP_RESP_OK);                 /* from the old address: the host matches it */
        s_route = f->payload[5];
    } else {                                        /* key, default address */
        s_def_route = f->payload[4];
        sbpdev_ack(f, SBP_RESP_OK);
    }
}

/* ---- dispatcher ----------------------------------------------------------------------------- */

static bool key_ok(const sbp_frame_t *f)
{
    return f->len >= 4 && sbp_get_u32(f->payload) == SBP_KEY_CONFIRM;
}

int sbpdev_handle(const sbp_frame_t *f)
{
    uint8_t type = sbp_type(f->mode), ver = sbp_ver(f->mode);
    switch (f->id) {
    case SBP_ID_VERSION:
        if (type == SBP_T_GETTING)
            send_version(ver);
        return 0;
    case SBP_ID_UART:
        handle_uart(f);
        return 0;
    case SBP_ID_MARK:
        if (type == SBP_T_SETTING) {
            if (!key_ok(f)) {
                sbpdev_ack(f, SBP_RESP_ERR_KEY);
                return 0;
            }
            s_mark = 1;
            sbpdev_ack(f, SBP_RESP_OK);
        }
        sbpdev_send(SBP_T_CONTENT, 0, SBP_ID_MARK, &s_mark, 1);
        return 0;
    case SBP_ID_FLASH:
        if (type != SBP_T_SETTING)
            return 0;
        if (!key_ok(f)) {
            sbpdev_ack(f, SBP_RESP_ERR_KEY);
            return 0;
        }
        if (ver == 0)
            sbpdev_ack(f, settings_save() ? SBP_RESP_OK : SBP_RESP_ERR_RUNTIME);
        else if (ver == 1) {
            settings_load(false); /* baud from flash applies at the next boot, as on the sonars */
            manager_report_restart(); /* the report period may have changed */
            sbpdev_ack(f, SBP_RESP_OK);
        } else if (ver == 2)
            sbpdev_ack(f, settings_erase() ? SBP_RESP_OK : SBP_RESP_ERR_RUNTIME);
        else
            sbpdev_ack(f, SBP_RESP_ERR_VERSION);
        return 0;
    case SBP_ID_BOOT:
        if (type != SBP_T_SETTING)
            return 0;
        if (!key_ok(f)) {
            sbpdev_ack(f, SBP_RESP_ERR_KEY);
            return 0;
        }
        if (ver == 0) {
            /* KoggerApp's reboot and the first step of its upgrade. Like a Kogger bootloader, open a
             * window for the first ID_UPDATE chunk; without one the module reboots (ota_tick). */
            sbpdev_ack(f, SBP_RESP_OK);
            ota_boot_request();
        } else if (ver == 1) {                      /* run firmware: boots a received, validated image */
            if (s_chan.kind == CH_NET) {
                sbpdev_ack(f, SBP_RESP_ERR_RUNTIME); /* firmware only over a UART: see ID_UPDATE */
                return 0;
            }
            bool reboot;
            sbpdev_ack(f, ota_run_request(&reboot));
            if (reboot)
                ota_reboot_into_new();
        } else {
            sbpdev_ack(f, SBP_RESP_ERR_VERSION);
        }
        return 0;
    case SBP_ID_UPDATE:
        /* SBP has no authentication and the image is not signed: a new firmware is accepted only from
         * a UART line (physical access), never from a network peer (docs/UPDATE.md). */
        if (s_chan.kind == CH_NET)
            sbpdev_ack(f, SBP_RESP_ERR_RUNTIME);
        else
            ota_on_update(f);
        return 0;
    default:
        return -1;
    }
}
