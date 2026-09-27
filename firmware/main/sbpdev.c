#include "sbpdev.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "link.h"
#include "manager.h"
#include "netcfg.h"
#include "ota.h"
#include "portinfo.h"
#include "ports.h"
#include "relay.h"
#include "uline.h"

static const char *TAG = "sbpdev";

#define REPORT_MS_DEFAULT 1000
#define SUB_TTL_US (60LL * 1000 * 1000) /* a channel silent this long no longer gets unsolicited frames */

static volatile uint8_t s_route;      /* own bus address (frame byte 2) */
static uint8_t s_mark;                /* set by ID_MARK, cleared by reboot: tells the host we kept its settings */
static uint16_t s_report_ms = REPORT_MS_DEFAULT;
/* Manager task only. Answers go to the request's channel; unsolicited frames go to every subscriber:
 * each port, and the last network peer, that sent the module a request within SUB_TTL_US. */
static sbp_chan_t s_reply;
static int64_t s_sub_port[2];
static sbp_chan_t s_sub_net;
static int64_t s_sub_net_t;
static uint32_t s_boot_baud;          /* port 0 rate for sbpdev_start(): one-shot resume, else saved; 0 none */

/* ---- settings in NVS ------------------------------------------------------------------------ */

static void settings_load(bool with_baud)
{
    nvs_handle_t h;
    if (nvs_open("wb", NVS_READONLY, &h) != ESP_OK)
        return;
    uint16_t ms;
    uint32_t baud;
    if (nvs_get_u16(h, "rep_ms", &ms) == ESP_OK)
        s_report_ms = ms;
    if (with_baud && nvs_get_u32(h, "baud", &baud) == ESP_OK)
        s_boot_baud = baud;
    nvs_close(h);
}

/* ID_FLASH v0: the report period and the current rates of both ports (ports.c). */
static bool settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("wb", NVS_READWRITE, &h) != ESP_OK)
        return false;
    bool ok = nvs_set_u16(h, "rep_ms", s_report_ms) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ports_save_all() && ok;
}

static bool settings_erase(void)
{
    nvs_handle_t h;
    if (nvs_open("wb", NVS_READWRITE, &h) != ESP_OK)
        return false;
    nvs_erase_key(h, "route"); /* boot address of 0.11 and older, unused since 0.12 */
    nvs_erase_key(h, "rep_ms");
    bool ok = nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ports_erase_saved() && ok;
}

void sbpdev_save_resume(void)
{
    nvs_handle_t h;
    if (nvs_open("wb", NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_set_u32(h, "rtbaud", link_baud());
    nvs_set_u8(h, "rtroute", s_route); /* unused by 0.12; a 0.11 image booted by this reboot comes up here */
    nvs_commit(h);
    nvs_close(h);
}

/* One-shot values written just before an update or role reboot win over the saved ones, then vanish. */
static void resume_load(void)
{
    nvs_handle_t h;
    if (nvs_open("wb", NVS_READWRITE, &h) != ESP_OK)
        return;
    uint32_t baud;
    if (nvs_get_u32(h, "rtbaud", &baud) == ESP_OK) {
        s_boot_baud = baud;
        nvs_erase_key(h, "rtbaud");
    }
    nvs_erase_key(h, "rtroute"); /* for a 0.11 target only: the address is fixed now */
    nvs_commit(h);
    nvs_close(h);
}

uint8_t sbpdev_route(void)
{
    return s_route;
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
    ports_load();
    s_route = netcfg_addr();
    resume_load();
}

void sbpdev_start(void)
{
    if (s_boot_baud && s_boot_baud != link_baud())
        link_set_baud(s_boot_baud);
    ESP_LOGI(TAG, "SBP address %u, report %u ms, baud %lu", s_route, s_report_ms, (unsigned long)link_baud());
}

/* ---- channels and framing ------------------------------------------------------------------- */

int sbpdev_chan_port(const sbp_chan_t *ch)
{
    return ch->kind == CH_LINK ? 0 : ch->kind == CH_LINE1 ? 1 : -1;
}

void sbpdev_begin_request(const sbp_chan_t *ch)
{
    int64_t now = esp_timer_get_time();
    s_reply = *ch;
    int port = sbpdev_chan_port(ch);
    if (port >= 0) {
        s_sub_port[port] = now;
    } else if (ch->kind == CH_NET) {
        s_sub_net = *ch;
        s_sub_net_t = now;
    }
}

void sbpdev_end_request(void)
{
    s_reply.kind = CH_NONE;
}

void sbpdev_set_channel(const sbp_chan_t *ch)
{
    s_reply = *ch;
}

void sbpdev_channel(sbp_chan_t *ch)
{
    *ch = s_reply;
}

static bool live(int64_t t, int64_t now)
{
    return t != 0 && now - t < SUB_TTL_US;
}

bool sbpdev_port_subscribed(int port)
{
    return (port == 0 || port == 1) && live(s_sub_port[port], esp_timer_get_time());
}

bool sbpdev_has_listener(void)
{
    int64_t now = esp_timer_get_time();
    return (live(s_sub_port[0], now) && link_proto() == LINK_PROTO_SBP) || live(s_sub_port[1], now) ||
           live(s_sub_net_t, now);
}

static void send_on(const sbp_chan_t *ch, uint8_t mode, uint8_t id, const uint8_t *payload, uint8_t len)
{
    uint8_t f[SBP_FRAME_MAX];
    switch (ch->kind) {
    case CH_LINK:
        if (link_proto() == LINK_PROTO_SBP) { /* a SLIP host does not read SBP */
            link_send_sbp(s_route, mode, id, payload, len);
            portinfo_module_tx(0);
        }
        break;
    case CH_LINE1:
        if (uline_tx_frame(f, sbp_encode(s_route, mode, id, payload, len, f)))
            portinfo_module_tx(1);
        break;
    case CH_NET:
        relay_send_to(ch->line, ch->ip, ch->port, f, sbp_encode(s_route, mode, id, payload, len, f));
        break;
    default:
        break;
    }
}

/* Every frame of the module leaves here, from the manager task only. */
static void emit(uint8_t mode, uint8_t id, const uint8_t *payload, uint8_t len)
{
    if (s_reply.kind != CH_NONE) {
        send_on(&s_reply, mode, id, payload, len);
        return;
    }
    int64_t now = esp_timer_get_time();
    static const sbp_chan_t PORT_CH[2] = { { .kind = CH_LINK }, { .kind = CH_LINE1, .line = 1 } };
    for (int p = 0; p < 2; p++)
        if (live(s_sub_port[p], now))
            send_on(&PORT_CH[p], mode, id, payload, len);
    if (live(s_sub_net_t, now))
        send_on(&s_sub_net, mode, id, payload, len);
}

void sbpdev_send(uint8_t type, uint8_t ver, uint8_t id, const uint8_t *payload, uint8_t len)
{
    emit(SBP_MODE(type, ver, 0, s_mark), id, payload, len);
}

void sbpdev_notify(uint8_t type, uint8_t ver, uint8_t id, const uint8_t *payload, uint8_t len)
{
    sbp_chan_t keep = s_reply;
    s_reply.kind = CH_NONE; /* to every subscriber, the requester included (it subscribed by asking) */
    emit(SBP_MODE(type, ver, 0, s_mark), id, payload, len);
    s_reply = keep;
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

/* ---- RX side (UART RX tasks, network task) --------------------------------------------------- */

void sbpdev_on_frame_ch(const sbp_frame_t *f, const sbp_chan_t *ch)
{
    uint8_t t = sbp_type(f->mode);
    bool own = f->route == s_route && (t == SBP_T_SETTING || t == SBP_T_GETTING);
    if (!own && !sbp_is_discovery(f->route, f->mode, f->id))
        return; /* another device's frame, or a CONTENT echo */
    if (ch->kind == CH_LINK)
        link_set_proto(LINK_PROTO_SBP);
    manager_post_sbp(f, ch);
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

/* The port of ID_UART v0 is the one behind the asking channel: the UART it came through, or for a
 * network peer the UART of the line whose socket received it. The uart id byte is ignored (KoggerApp
 * always sends 1). */
static int chan_line(void)
{
    int port = sbpdev_chan_port(&s_reply);
    return port >= 0 ? port : s_reply.kind == CH_NET ? s_reply.line : 0;
}

static void send_uart(uint8_t ver)
{
    uint8_t p[9];
    sbp_put_u32(p, SBP_KEY_CONFIRM);
    if (ver == 0) {
        p[4] = 1;
        sbp_put_u32(&p[5], ports_baud(chan_line()));
        sbpdev_send(SBP_T_CONTENT, 0, SBP_ID_UART, p, 9);
    } else if (ver == 1) {
        p[4] = 1;
        p[5] = s_route;
        sbpdev_send(SBP_T_CONTENT, 1, SBP_ID_UART, p, 6);
    } else if (ver == 2) {
        p[4] = s_route;
        sbpdev_send(SBP_T_CONTENT, 2, SBP_ID_UART, p, 5);
    }
}

static bool addr_ok(uint8_t a)
{
    return a != SBP_ROUTE_BCAST0 && a != SBP_ROUTE_BCAST;
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
        if (!ports_baud_ok(baud)) {
            sbpdev_ack(f, SBP_RESP_ERR_PAYLOAD);
            return;
        }
        sbpdev_ack(f, SBP_RESP_OK);                 /* leaves at the old rate: queued before the switch */
        if (!ports_set_baud(chan_line(), baud, sbpdev_chan_port(&s_reply) >= 0))
            ESP_LOGW(TAG, "rate %lu not applied", (unsigned long)baud);
    } else {                                        /* v1 {key, uart id, address}, v2 {key, address} */
        size_t at = ver == 1 ? 5 : 4;
        if (f->len < at + 1 || !addr_ok(f->payload[at])) {
            sbpdev_ack(f, SBP_RESP_ERR_PAYLOAD);
            return;
        }
        sbpdev_ack(f, SBP_RESP_OK);                 /* from the old address: the host matches it */
        relay_set_addr(f->payload[at]);             /* saved: the one own address (ID_WIFI_NET v6) */
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
            settings_load(false); /* the report period; saved rates apply at the next boot */
            manager_report_restart();
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
            if (s_reply.kind == CH_NET) {
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
        if (s_reply.kind == CH_NET)
            sbpdev_ack(f, SBP_RESP_ERR_RUNTIME);
        else
            ota_on_update(f);
        return 0;
    default:
        return -1;
    }
}
