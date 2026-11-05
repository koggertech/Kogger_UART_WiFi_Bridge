/*
 * profctl.c - ID_WIFI_PROFILE 0x5B (profctl.h). The upload is staged in RAM and nothing is saved until the whole
 * profile is in, its checksum matches and every field passes the same checks a single setting does; then every
 * setting is saved, the fingerprint of what is now saved goes to NVS "prof" with the profile's name, and the module
 * reboots into it. The fingerprint is taken from the settings as saved (not from the blob): a value the module
 * stores in its own way (a power above 20 dBm, a password of an open network) does not read as a change later.
 */
#include "profctl.h"

#include <ctype.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "manager.h"
#include "netcfg.h"
#include "ota.h"
#include "ports.h"
#include "profile.h"
#include "radio.h"
#include "sbpdev.h"

static const char *TAG = "profile";

enum { V_STATUS = 0, V_BEGIN = 1, V_CHUNK = 2, V_APPLY = 3 };

typedef struct {
    uint32_t crc;      /* fingerprint of the settings as the profile left them */
    char     name[33];
} meta_t;

static struct {
    uint8_t  buf[PROFILE_MAX];
    uint16_t len, got;  /* announced, received in order */
    uint32_t crc;       /* announced */
    bool     open;
    uint8_t  last;      /* why the last apply was refused (profile.h PROFILE_E_*), 0 none */
} U;

static profile_t s_p;               /* static: the manager task's stack is 6 KB, a profile about 1 KB */
static uint8_t   s_enc[PROFILE_MAX];

static bool meta_load(meta_t *m)
{
    nvs_handle_t h;
    size_t sz = sizeof *m;
    bool ok = false;
    if (nvs_open("wb", NVS_READONLY, &h) == ESP_OK) {
        ok = nvs_get_blob(h, "prof", m, &sz) == ESP_OK && sz == sizeof *m && memchr(m->name, 0, sizeof m->name);
        nvs_close(h);
    }
    return ok;
}

static bool meta_save(const meta_t *m)
{
    nvs_handle_t h;
    if (nvs_open("wb", NVS_READWRITE, &h) != ESP_OK)
        return false;
    bool ok = nvs_set_blob(h, "prof", m, sizeof *m) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

static void line_in(profile_line_t *o, const line_cfg_t *l)
{
    o->mode = l->mode;
    o->dest = l->dest;
    memcpy(o->ip, l->ip, 4);
    o->rport = l->rport;
    o->lport = l->lport;
}

/* The saved settings as a profile named `name`, and their fingerprint. */
static uint32_t fingerprint(const char *name)
{
    profile_t *p = &s_p;
    memset(p, 0, sizeof *p);
    strlcpy(p->name, name, sizeof p->name);
    p->role = (uint8_t)netcfg_saved_role();
    p->address = netcfg_addr();
    p->x1_baud = ports_saved_baud(0);
    radio_cfg_t rc;
    radio_now_t rn;
    radio_get(&rc, &rn);
    p->radio[0] = rc.power;
    p->radio[1] = rc.proto;
    p->radio[2] = rc.bw;
    p->radio[3] = rc.ps;
    ap_cfg_t a;
    netcfg_ap(&a);
    p->ap_channel = a.channel;
    p->ap_hidden = a.hidden;
    p->ap_max_clients = a.max_clients;
    p->ap_auth = a.auth;
    strlcpy(p->ap_ssid, a.ssid, sizeof p->ap_ssid);
    strlcpy(p->ap_pass, a.pass, sizeof p->ap_pass);
    ip_cfg_t ip;
    netcfg_ip(&ip);
    memcpy(p->ip, ip.ip, 4);
    memcpy(p->mask, ip.mask, 4);
    p->dhcp = ip.dhcp;
    memcpy(p->pool_start, ip.pool_start, 4);
    memcpy(p->pool_end, ip.pool_end, 4);
    p->lease_min = ip.lease_min;
    p->offer = ip.offer;
    line_cfg_t l;
    netcfg_line(0, &l);
    line_in(&p->line[0], &l);
    netcfg_line(1, &l);
    line_in(&p->line[1], &l);
    p->x2_baud = l.baud;
    p->tx1 = l.tx_pin;
    p->rx1 = l.rx_pin;
    p->xlat1 = netcfg_line1_xlat();
    p->nnets = (uint8_t)manager_known_get(p->nets, PROFILE_NETS);
    size_t n = profile_encode(p, s_enc, sizeof s_enc);
    return profile_crc32(s_enc, n);
}

/* CONTENT v0 {U1 flags: bit0 a profile was loaded, bit1 settings changed since; U4 fingerprint; U2 received,
 * U2 announced (the upload in progress); U1 name length, name}. */
static void send_status(void)
{
    meta_t m;
    bool has = meta_load(&m);
    uint8_t q[10 + 32];
    size_t nl = has ? strnlen(m.name, 32) : 0;
    q[0] = (uint8_t)((has ? 1 : 0) | (has && fingerprint(m.name) != m.crc ? 2 : 0));
    sbp_put_u32(q + 1, has ? m.crc : 0);
    sbp_put_u16(q + 5, U.open ? U.got : 0);
    sbp_put_u16(q + 7, U.open ? U.len : 0);
    q[9] = (uint8_t)nl;
    memcpy(q + 10, m.name, nl);
    sbpdev_send(SBP_T_CONTENT, V_STATUS, SBP_ID_WIFI_PROFILE, q, (uint8_t)(10 + nl));
}

/* CONTENT v3 {U1 reason} after an apply (0 = saved, the module reboots). */
static void send_reason(void)
{
    sbpdev_send(SBP_T_CONTENT, V_APPLY, SBP_ID_WIFI_PROFILE, &U.last, 1);
}

static bool key_ok(const sbp_frame_t *f)
{
    return f->len >= 4 && sbp_get_u32(f->payload) == SBP_KEY_CONFIRM;
}

/* Saved networks are kept only after a successful join elsewhere; a profile's are saved unjoined, so each must
 * be one the station can use: no two of one name, a password empty (open) or 8..63 characters or 64 hex digits. */
static bool nets_ok(const profile_t *p)
{
    for (int i = 0; i < p->nnets; i++) {
        const char *pw = p->nets[i].pass;
        size_t pl = strlen(pw);
        if (pl == 64) {
            for (size_t k = 0; k < pl; k++)
                if (!isxdigit((unsigned char)pw[k]))
                    return false;
        } else if (pl != 0 && pl < 8) {
            return false;
        }
        for (int k = 0; k < i; k++)
            if (!strcmp(p->nets[k].ssid, p->nets[i].ssid))
                return false;
    }
    return true;
}

/* Every field against the module's rules, into the setting types; PROFILE_OK or why not. */
static int check(const profile_t *p, ap_cfg_t *a, ip_cfg_t *ip, line_cfg_t l[2], radio_cfg_t *rc)
{
    if (!ports_baud_ok(p->x1_baud))
        return PROFILE_E_X1_BAUD;
    *rc = (radio_cfg_t){ p->radio[0], p->radio[1], p->radio[2], p->radio[3] };
    if (!radio_cfg_ok(rc))
        return PROFILE_E_RADIO;
    memset(a, 0, sizeof *a);
    strlcpy(a->ssid, p->ap_ssid, sizeof a->ssid);
    if (p->ap_auth != 0) /* an open network keeps no password (as ID_WIFI_NET v1) */
        strlcpy(a->pass, p->ap_pass, sizeof a->pass);
    a->channel = p->ap_channel;
    a->hidden = p->ap_hidden;
    a->max_clients = p->ap_max_clients;
    a->auth = p->ap_auth;
    if (!netcfg_ap_ok(a))
        return PROFILE_E_AP;
    memcpy(ip->ip, p->ip, 4);
    memcpy(ip->mask, p->mask, 4);
    ip->dhcp = p->dhcp;
    memcpy(ip->pool_start, p->pool_start, 4);
    memcpy(ip->pool_end, p->pool_end, 4);
    ip->lease_min = p->lease_min;
    ip->offer = p->offer;
    if (!netcfg_ip_ok(ip))
        return PROFILE_E_IP;
    for (int i = 0; i < 2; i++) {
        netcfg_line(i, &l[i]); /* line 0 keeps the build's pins and its rate field (X1's rate is x1_baud) */
        l[i].mode = p->line[i].mode;
        l[i].dest = p->line[i].dest;
        memcpy(l[i].ip, p->line[i].ip, 4);
        l[i].rport = p->line[i].rport;
        l[i].lport = p->line[i].lport;
    }
    l[1].baud = p->x2_baud;
    l[1].tx_pin = p->tx1;
    l[1].rx_pin = p->rx1;
    int lc = netcfg_lines_check(l);
    if (lc)
        return lc == 1 ? PROFILE_E_LINE0 : lc == 2 ? PROFILE_E_LINE1 : PROFILE_E_LINES;
    if (p->xlat1 > LINE_XLAT_TELEMETRY || (p->xlat1 && !(netcfg_xlat_built() & (1u << (p->xlat1 - 1)))))
        return PROFILE_E_XLAT;
    if (!nets_ok(p))
        return PROFILE_E_NETS;
    return PROFILE_OK;
}

static void apply(const sbp_frame_t *f)
{
    ap_cfg_t a;
    ip_cfg_t ip;
    line_cfg_t l[2];
    radio_cfg_t rc;
    profile_t *p = &s_p;
    if (!U.open || U.got != U.len)
        U.last = PROFILE_E_INCOMPLETE;
    else if (profile_crc32(U.buf, U.len) != U.crc)
        U.last = PROFILE_E_CRC;
    else if ((U.last = (uint8_t)profile_decode(U.buf, U.len, p)) == PROFILE_OK)
        U.last = (uint8_t)check(p, &a, &ip, l, &rc);
    if (U.last == PROFILE_OK && (ota_pending() || ota_boot_mode() != 0))
        U.last = PROFILE_E_BUSY; /* a reboot now would roll a fresh image back, or end a transfer */
    if (U.last != PROFILE_OK) {
        ESP_LOGW(TAG, "profile refused: reason %u", U.last);
        sbpdev_ack(f, U.last == PROFILE_E_BUSY ? SBP_RESP_ERR_RUNTIME : SBP_RESP_ERR_PAYLOAD);
        send_reason();
        return;
    }
    char name[33];
    strlcpy(name, p->name, sizeof name);
    bool ok = netcfg_set_role((wifi_role_t)p->role) && netcfg_set_ap(&a) && netcfg_set_ip(&ip) &&
              netcfg_set_lines(l) && netcfg_set_baud1(p->x2_baud) && netcfg_set_line1_xlat(p->xlat1) &&
              netcfg_set_addr(p->address) && ports_save_baud(0, p->x1_baud) && radio_store(&rc) &&
              manager_known_set(p->nets, p->nnets);
    meta_t m = { 0 };
    if (ok) {
        strlcpy(m.name, name, sizeof m.name);
        m.crc = fingerprint(name);
        ok = meta_save(&m);
    }
    U.open = false;
    if (!ok) { /* a write refused part way (flash): what was saved stays, nothing reboots, the host is told */
        ESP_LOGE(TAG, "profile \"%s\" NOT saved whole", name);
        sbpdev_ack(f, SBP_RESP_ERR_RUNTIME);
        return;
    }
    ESP_LOGW(TAG, "profile \"%s\" saved (%08lx): rebooting into it", name, (unsigned long)m.crc);
    sbpdev_ack(f, SBP_RESP_OK);
    send_reason();
    send_status();
    vTaskDelay(pdMS_TO_TICKS(200)); /* the answers leave first: they may go over the radio the reboot stops */
    esp_restart();
}

void profctl_handle(const sbp_frame_t *f)
{
    uint8_t ver = sbp_ver(f->mode);
    bool set = sbp_type(f->mode) == SBP_T_SETTING;
    const uint8_t *q = f->payload;
    switch (ver) {
    case V_STATUS:
        if (set)
            sbpdev_ack(f, SBP_RESP_ERR_TYPE);
        else
            send_status();
        break;
    case V_BEGIN: { /* SETTING {KEY, U2 length, U4 CRC-32}: a new upload; the earlier one is dropped */
        if (!set) {
            sbpdev_ack(f, SBP_RESP_ERR_TYPE);
            break;
        }
        if (!key_ok(f)) {
            sbpdev_ack(f, SBP_RESP_ERR_KEY);
            break;
        }
        uint16_t len = f->len == 10 ? sbp_get_u16(q + 4) : 0;
        if (len == 0 || len > PROFILE_MAX) {
            sbpdev_ack(f, SBP_RESP_ERR_PAYLOAD);
            break;
        }
        U.len = len;
        U.got = 0;
        U.crc = sbp_get_u32(q + 6);
        U.open = true;
        sbpdev_ack(f, SBP_RESP_OK);
        break;
    }
    case V_CHUNK: { /* SETTING {U2 offset, bytes}: in order; a piece sent again (its answer was lost) is OK */
        if (!set) {
            sbpdev_ack(f, SBP_RESP_ERR_TYPE);
            break;
        }
        if (!U.open) {
            sbpdev_ack(f, SBP_RESP_ERR_RUNTIME);
            break;
        }
        uint16_t off = f->len >= 3 ? sbp_get_u16(q) : 0xFFFF;
        size_t k = f->len >= 3 ? f->len - 2u : 0;
        if (k && off == U.got && U.got + k <= U.len) {
            memcpy(U.buf + U.got, q + 2, k);
            U.got = (uint16_t)(U.got + k);
            sbpdev_ack(f, SBP_RESP_OK);
        } else if (k && off + k <= U.got && !memcmp(U.buf + off, q + 2, k)) {
            sbpdev_ack(f, SBP_RESP_OK);
        } else {
            sbpdev_ack(f, SBP_RESP_ERR_PAYLOAD);
        }
        break;
    }
    case V_APPLY: /* SETTING {KEY}: check whole, save, reboot; GETTING: the reason of the last refusal */
        if (!set) {
            send_reason();
            break;
        }
        if (!key_ok(f)) {
            sbpdev_ack(f, SBP_RESP_ERR_KEY);
            break;
        }
        apply(f);
        break;
    default:
        sbpdev_ack(f, SBP_RESP_ERR_VERSION);
    }
}
