#include "manager.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "bridge.h"
#include "frame.h"
#include "link.h"
#include "netcfg.h"
#include "netctl.h"
#include "ota.h"
#include "portinfo.h"
#include "ports.h"
#include "proto.h"
#include "radio.h"
#include "relay.h"
#include "sbp.h"
#include "sbpdev.h"
#include "wifiap.h"
#include "wifistat.h"

#define PROTO_VERSION   1
#define MAX_KNOWN       8
#define MAX_SCAN        32
#define CONNECT_RETRIES 3
#define SEARCH_PERIOD_US (15 * 1000 * 1000)
#define QUEUE_LEN       64                       /* a host's burst of requests fits (KoggerApp opens with ~30) */
#define EVENT_RESERVE   8                        /* queue slots only Wi-Fi events and timers may take */
#define REQ_WAIT_TICKS  2                        /* 20 ms a request waits for room before it is dropped */
#define CONNECT_STUCK_US (30LL * 1000 * 1000)    /* supervision limits (docs/DESIGN.md, 0.11) */
#define SCAN_STUCK_US   (15LL * 1000 * 1000)
#define AP_LOST_TICKS   4                        /* 2 s of 500 ms ticks without an association */

static const char *TAG = "mgr";

/* ---- state ---------------------------------------------------------------- */

typedef enum { ST_IDLE, ST_SEARCHING, ST_CONNECTING, ST_CONNECTED, ST_AP } state_t;
static const char *const ST_NAME[] = { "IDLE", "SEARCHING", "CONNECTING", "CONNECTED", "AP" };

typedef struct {
    char     ssid[33];
    char     pass[65];
    uint32_t seq;   /* last successful use, higher = newer */
} known_t;

typedef enum {
    M_LINE, M_SBP, M_SCAN_DONE, M_DISCONNECTED, M_GOT_IP, M_TIMER, M_REPORT, M_OTA_TICK,
    M_AP_START, M_AP_STOP, M_AP_CLIENT
} msg_type_t;

enum { CL_LEFT, CL_JOINED, CL_ADDRESS }; /* M_AP_CLIENT arg */

typedef struct {
    msg_type_t type;
    int        arg;      /* disconnect reason; CL_* */
    void      *buf;      /* M_LINE: NUL-terminated text; M_SBP: sbp_blob_t. malloc'd */
    char       ssid[33]; /* M_DISCONNECTED */
    uint8_t    mac[6];   /* M_AP_CLIENT */
    uint32_t   ip;       /* M_AP_CLIENT, network order */
} msg_t;

typedef struct {
    sbp_frame_t f;       /* f.payload points at payload[] below */
    sbp_chan_t  ch;      /* where it came from: the answers go there */
    uint8_t     payload[];
} sbp_blob_t;

static struct {
    state_t  st;
    bool     autoc;          /* reconnect / search for saved networks */
    char     ssid[33];       /* current target */
    char     pass[65];
    bool     user_target;    /* target came from CONNECT (not from the saved list) */
    bool     save_on_ip;
    int      retries;
    int      last_reason;
    bool     expect_disc;    /* we called esp_wifi_disconnect(); swallow its event */
    bool     scanning;
    bool     scan_for_user;  /* text SCAN waiting for the result */
    char     scan_tag[16];
    bool     scan_for_sbp;   /* ID_WIFI v2 waiting for the result */
    sbp_frame_t scan_req;    /* that request (header only), to reject it if the scan is cancelled */
    sbp_chan_t scan_ch;      /* where that request came from: the results go there */
    int64_t  scan_since;
    int64_t  since;          /* when the state was last set */
    int      ap_lost;        /* ticks in CONNECTED without an association */
    known_t  known[MAX_KNOWN];
    int      nknown;
    uint32_t seq;
} S;

static QueueHandle_t s_q;
static volatile uint32_t s_req_drops; /* requests dropped for a full queue: ID_WIFI v1 (room_for_request) */
static esp_netif_t *s_net;            /* station or access point interface */
static bool s_ap;                     /* running as access point (the role is fixed until reboot) */
static esp_timer_handle_t s_timer;    /* search retry */
static esp_timer_handle_t s_report;   /* ID_WIFI v1 report */
static esp_timer_handle_t s_otatick; /* update window/timeouts/self-confirmation */

/* ---- text output ------------------------------------------------------------ */

static void ctl_send(const char *fmt, ...)
{
    if (s_ap && link_proto() != LINK_PROTO_SLIP)
        return; /* an access point may have a device on its host port: text only to a SLIP host */
    char buf[420];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof buf)
        n = sizeof buf - 1;
    link_send(FRAME_T_CTL, (const uint8_t *)buf, (size_t)n);
}

static const char *enc(const char *s, char *out, size_t cap)
{
    if (pct_encode((const uint8_t *)s, strlen(s), out, cap) < 0)
        out[0] = '\0';
    return out;
}

/* ---- reason / auth / reset names and SBP codes ----------------------------------- */

enum { WHY_NONE, WHY_NO_AP, WHY_AUTH, WHY_ASSOC, WHY_LOST, WHY_OTHER };
static const char *const WHY_NAME[] = { "NONE", "NO_AP", "AUTH", "ASSOC", "LOST", "OTHER" };

static int why_code(int reason)
{
    switch (reason) {
    case 0: return WHY_NONE;
    case WIFI_REASON_NO_AP_FOUND:
    case 210: case 211: case 212: /* NO_AP_FOUND_W_COMPATIBLE_SECURITY / _IN_AUTHMODE_THRESHOLD / _IN_RSSI_THRESHOLD */
        return WHY_NO_AP;
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return WHY_AUTH;
    case WIFI_REASON_ASSOC_FAIL:
        return WHY_ASSOC;
    case WIFI_REASON_BEACON_TIMEOUT:
        return WHY_LOST;
    default:
        return WHY_OTHER;
    }
}

static const char *why(int reason)
{
    return WHY_NAME[why_code(reason)];
}

/* SBP auth codes (docs/SBP_WIFI.md): index into this table, 255 = other. */
static const char *const AUTH_NAME[] = { "OPEN", "WEP", "WPA", "WPA2", "WPA/WPA2", "WPA3", "WPA2/WPA3",
                                         "ENTERPRISE", "OWE" };

static uint8_t auth_code(wifi_auth_mode_t a)
{
    switch (a) {
    case WIFI_AUTH_OPEN:            return 0;
    case WIFI_AUTH_WEP:             return 1;
    case WIFI_AUTH_WPA_PSK:         return 2;
    case WIFI_AUTH_WPA2_PSK:        return 3;
    case WIFI_AUTH_WPA_WPA2_PSK:    return 4;
    case WIFI_AUTH_WPA3_PSK:        return 5;
    case WIFI_AUTH_WPA2_WPA3_PSK:   return 6;
    case WIFI_AUTH_WPA2_ENTERPRISE: return 7;
    case WIFI_AUTH_OWE:             return 8;
    default:                        return 255;
    }
}

static const char *auth_name(wifi_auth_mode_t a)
{
    uint8_t c = auth_code(a);
    return c < sizeof AUTH_NAME / sizeof AUTH_NAME[0] ? AUTH_NAME[c] : "OTHER";
}

static const char *reset_name(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "POWERON";
    case ESP_RST_EXT:      return "EXT";
    case ESP_RST_SW:       return "SW";
    case ESP_RST_PANIC:    return "PANIC";
    case ESP_RST_INT_WDT:  return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT:      return "WDT";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_USB:      return "USB";
    case ESP_RST_JTAG:     return "JTAG";
    default:               return "OTHER";
    }
}

static int known_find(const char *ssid);

/* ---- status snapshot (shared by the text and SBP encodings) ------------------------- */

typedef struct {
    const char         *ssid;     /* target while connecting/connected, own network as AP, else "" */
    int                 rssi, ch; /* AP: weakest client */
    uint8_t             auth;     /* SBP auth code, 255 = unknown */
    uint8_t             bssid[6]; /* AP: own MAC */
    esp_netif_ip_info_t ipi;
    esp_ip4_addr_t      dns;
    wifistat_t          ws;
    int                 clients;  /* AP */
    char                own[33];
} status_t;

/* The weakest client decides whether the bridge holds. */
static int weakest(const ap_client_t *cl, int n)
{
    int r = 0;
    for (int i = 0; i < n; i++)
        if (i == 0 || cl[i].rssi < r)
            r = cl[i].rssi;
    return r;
}

static void snapshot_ap(status_t *s)
{
    ap_cfg_t a;
    netcfg_ap(&a);
    strlcpy(s->own, a.ssid, sizeof s->own);
    s->ssid = S.st == ST_AP ? s->own : "";
    s->ch = a.channel;
    s->auth = a.auth == 0 ? 0 : a.auth == 1 ? 3 : 6; /* OPEN, WPA2, WPA2/WPA3 in SBP codes */
    esp_wifi_get_mac(WIFI_IF_AP, s->bssid);
    if (S.st == ST_AP) {
        ap_client_t cl[10];
        s->clients = wifiap_clients(cl, 10);
        s->rssi = weakest(cl, s->clients);
        esp_netif_get_ip_info(s_net, &s->ipi);
    }
}

static void snapshot(status_t *s)
{
    memset(s, 0, sizeof *s);
    s->ssid = (S.st == ST_CONNECTING || S.st == ST_CONNECTED) ? S.ssid : "";
    s->auth = 255;
    if (s_ap)
        snapshot_ap(s);
    if (S.st == ST_CONNECTED) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            memcpy(s->bssid, ap.bssid, 6);
            s->rssi = ap.rssi;
            s->ch = ap.primary;
            s->auth = auth_code(ap.authmode);
        }
        esp_netif_get_ip_info(s_net, &s->ipi);
        esp_netif_dns_info_t d;
        if (esp_netif_get_dns_info(s_net, ESP_NETIF_DNS_MAIN, &d) == ESP_OK && d.ip.type == ESP_IPADDR_TYPE_V4)
            s->dns = d.ip.u_addr.ip4;
    }
    wifistat_get(&s->ws);
}

static void status_fields(char *out, size_t cap)
{
    status_t s;
    snapshot(&s);
    char essid[100], bssid[18] = "", ip[16] = "", gw[16] = "", dns[16] = "";
    if (S.st == ST_CONNECTED || S.st == ST_AP) {
        snprintf(bssid, sizeof bssid, MACSTR, MAC2STR(s.bssid));
        snprintf(ip, sizeof ip, IPSTR, IP2STR(&s.ipi.ip));
        snprintf(gw, sizeof gw, IPSTR, IP2STR(&s.ipi.gw));
        snprintf(dns, sizeof dns, IPSTR, IP2STR(&s.dns));
    }
    snprintf(out, cap, "state=%s auto=%d ssid=%s bssid=%s ch=%d rssi=%d ip=%s gw=%s dns=%s reason=%d why=%s "
             "rx_bps=%lu tx_bps=%lu role=%s clients=%d",
             ST_NAME[S.st], S.autoc ? 1 : 0, enc(s.ssid, essid, sizeof essid), bssid, s.ch, s.rssi,
             ip, gw, dns, S.last_reason, why(S.last_reason), (unsigned long)s.ws.rx_bps, (unsigned long)s.ws.tx_bps,
             s_ap ? "ap" : "sta", s.clients);
}

/* ---- ID_WIFI encodings (docs/SBP_WIFI.md) -------------------------------------------- */

#define WIFI_V0_FIXED 39

static void sbp_send_status(bool notify)
{
    status_t s;
    snapshot(&s);
    uint8_t p[WIFI_V0_FIXED + 32];
    size_t sl = strlen(s.ssid);
    p[0] = (uint8_t)S.st;
    p[1] = (uint8_t)((S.autoc ? 1 : 0) | (S.scanning ? 2 : 0) | (s.ssid[0] && known_find(s.ssid) >= 0 ? 4 : 0));
    p[2] = (uint8_t)(int8_t)s.rssi;
    p[3] = (uint8_t)s.ch;
    p[4] = s.auth;
    p[5] = (uint8_t)S.last_reason;
    p[6] = (uint8_t)why_code(S.last_reason);
    p[7] = (uint8_t)s.clients;               /* access point role; 0 for a station */
    memcpy(&p[8], s.bssid, 6);
    memcpy(&p[14], &s.ipi.ip.addr, 4);       /* a.b.c.d: esp_ip4_addr_t is in network order */
    memcpy(&p[18], &s.ipi.netmask.addr, 4);
    memcpy(&p[22], &s.ipi.gw.addr, 4);
    memcpy(&p[26], &s.dns.addr, 4);
    sbp_put_u32(&p[30], s.ws.rx_bps);
    sbp_put_u32(&p[34], s.ws.tx_bps);
    p[38] = (uint8_t)sl;
    memcpy(&p[39], s.ssid, sl);
    (notify ? sbpdev_notify : sbpdev_send)(SBP_T_CONTENT, 0, SBP_ID_WIFI, p, (uint8_t)(WIFI_V0_FIXED + sl));
}

static void sbp_send_link(bool notify)
{
    wifistat_t ws;
    wifistat_get(&ws);
    int rssi = 0;
    wifi_ap_record_t ap;
    if (s_ap && S.st == ST_AP) {
        ap_client_t cl[10];
        rssi = weakest(cl, wifiap_clients(cl, 10));
    } else if (S.st == ST_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        rssi = ap.rssi;
    }
    uint8_t p[27];
    p[0] = (uint8_t)S.st;
    p[1] = (uint8_t)(int8_t)rssi;
    sbp_put_u32(&p[2], ws.rx_bps);
    sbp_put_u32(&p[6], ws.tx_bps);
    sbp_put_u32(&p[10], ws.rx_total);
    sbp_put_u32(&p[14], ws.tx_total);
    sbp_put_u32(&p[18], (uint32_t)(esp_timer_get_time() / 1000000));
    p[22] = (uint8_t)esp_reset_reason(); /* 0.11: why the module last restarted (docs/SBP_WIFI.md) */
    sbp_put_u32(&p[23], s_req_drops);    /* 0.14: requests dropped for a full queue, should stay 0 */
    (notify ? sbpdev_notify : sbpdev_send)(SBP_T_CONTENT, 1, SBP_ID_WIFI, p, sizeof p);
}

static void sbp_send_radio(bool notify)
{
    radio_cfg_t c;
    radio_now_t n;
    radio_get(&c, &n);
    uint8_t q[8] = { c.power, c.proto, c.bw, c.ps, n.power, n.phy, n.proto, n.bw };
    (notify ? sbpdev_notify : sbpdev_send)(SBP_T_CONTENT, 7, SBP_ID_WIFI, q, sizeof q);
}

static void sbp_send_saved(void)
{
    if (S.nknown == 0) {
        const uint8_t p[2] = { 0, 0 };
        sbpdev_send(SBP_T_CONTENT, 4, SBP_ID_WIFI, p, sizeof p);
        return;
    }
    for (int i = 0; i < S.nknown; i++) {
        uint8_t p[3 + 32];
        size_t sl = strlen(S.known[i].ssid);
        p[0] = (uint8_t)i;
        p[1] = (uint8_t)S.nknown;
        p[2] = (uint8_t)sl;
        memcpy(&p[3], S.known[i].ssid, sl);
        sbpdev_send(SBP_T_CONTENT, 4, SBP_ID_WIFI, p, (uint8_t)(3 + sl));
    }
}

static void report_restart(void)
{
    esp_timer_stop(s_report);
    uint16_t ms = sbpdev_report_ms();
    if (ms)
        esp_timer_start_periodic(s_report, (uint64_t)ms * 1000);
}

void manager_report_restart(void)
{
    report_restart();
}

static void set_state(state_t st)
{
    S.st = st;
    S.since = esp_timer_get_time();
    S.ap_lost = 0;
    relay_wifi(st == ST_CONNECTED || st == ST_AP);
    char f[380];
    status_fields(f, sizeof f);
    ctl_send("* STATE %s", f);
    if (sbpdev_has_listener())
        sbp_send_status(true);
    ESP_LOGI(TAG, "%s", f);
}

/* ---- saved networks -------------------------------------------------------- */

typedef struct {
    uint8_t  version;
    uint8_t  n;
    uint32_t seq;
    known_t  e[MAX_KNOWN];
} known_blob_t;

static void known_load(void)
{
    nvs_handle_t h;
    known_blob_t *b = calloc(1, sizeof *b);
    size_t sz = sizeof *b;
    esp_err_t e = ESP_ERR_NVS_NOT_FOUND; /* no namespace yet: nothing saved */
    if (nvs_open("wb", NVS_READONLY, &h) == ESP_OK) {
        e = b ? nvs_get_blob(h, "known", b, &sz) : ESP_ERR_NO_MEM;
        nvs_close(h);
    }
    if (e == ESP_OK && sz == sizeof *b && b->version == 1 && b->n <= MAX_KNOWN) {
        S.nknown = b->n;
        S.seq = b->seq;
        memcpy(S.known, b->e, sizeof S.known);
    } else if (e == ESP_ERR_NVS_NOT_FOUND) {
        /* A new or factory-reset module knows the factory network until a list is saved. Forgetting
         * every network saves an empty list, so the factory one does not come back by itself. */
        strlcpy(S.known[0].ssid, CONFIG_WB_FACTORY_SSID, sizeof S.known[0].ssid);
        strlcpy(S.known[0].pass, CONFIG_WB_FACTORY_PASS, sizeof S.known[0].pass);
        S.nknown = 1;
    }
    free(b);
    ESP_LOGI(TAG, "%d saved network(s)%s", S.nknown, e == ESP_ERR_NVS_NOT_FOUND ? ": the factory one" : "");
}

static void known_save(void)
{
    nvs_handle_t h;
    known_blob_t *b = calloc(1, sizeof *b);
    if (!b || nvs_open("wb", NVS_READWRITE, &h) != ESP_OK) {
        free(b);
        return;
    }
    b->version = 1;
    b->n = (uint8_t)S.nknown;
    b->seq = S.seq;
    memcpy(b->e, S.known, sizeof S.known);
    if (nvs_set_blob(h, "known", b, sizeof *b) != ESP_OK || nvs_commit(h) != ESP_OK)
        ESP_LOGE(TAG, "saving networks failed");
    nvs_close(h);
    free(b);
}

static int known_find(const char *ssid)
{
    for (int i = 0; i < S.nknown; i++)
        if (strcmp(S.known[i].ssid, ssid) == 0)
            return i;
    return -1;
}

static void known_put(const char *ssid, const char *pass)
{
    int i = known_find(ssid);
    if (i < 0) {
        if (S.nknown < MAX_KNOWN) {
            i = S.nknown++;
        } else { /* evict the least recently used */
            i = 0;
            for (int k = 1; k < S.nknown; k++)
                if (S.known[k].seq < S.known[i].seq)
                    i = k;
        }
    }
    strlcpy(S.known[i].ssid, ssid, sizeof S.known[i].ssid);
    strlcpy(S.known[i].pass, pass, sizeof S.known[i].pass);
    S.known[i].seq = ++S.seq;
    known_save();
}

static bool known_remove(const char *ssid)
{
    int i = known_find(ssid);
    if (i < 0)
        return false;
    S.known[i] = S.known[--S.nknown];
    memset(&S.known[S.nknown], 0, sizeof S.known[0]);
    known_save();
    return true;
}

/* ---- Wi-Fi actions ----------------------------------------------------------- */

static void timer_arm(uint64_t us)
{
    esp_timer_stop(s_timer);
    esp_timer_start_once(s_timer, us);
}

static bool scan_start(void)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, false) != ESP_OK)
        return false;
    S.scanning = true;
    S.scan_since = esp_timer_get_time();
    return true;
}

static void sbp_send_scan(const wifi_ap_record_t *recs, int n);
static void go_search(void);

/* The scan's answer goes to whoever asked for it; the current channel is restored afterwards. */
static void scan_answer(const wifi_ap_record_t *recs, int n, bool ok)
{
    sbp_chan_t cur;
    sbpdev_channel(&cur);
    sbpdev_set_channel(&S.scan_ch);
    if (ok)
        sbp_send_scan(recs, n);
    else
        sbpdev_ack(&S.scan_req, SBP_RESP_ERR_RUNTIME);
    sbpdev_set_channel(&cur);
    S.scan_for_sbp = false;
}

static void scan_failed(const char *why)
{
    if (S.scan_for_user) {
        ctl_send("%s ERR ABORTED %s", S.scan_tag, why);
        S.scan_for_user = false;
    }
    if (S.scan_for_sbp)
        scan_answer(NULL, 0, false);
}

static void drop_connection(void)
{
    if (S.st == ST_CONNECTING || S.st == ST_CONNECTED) {
        S.expect_disc = true;
        esp_wifi_disconnect();
    }
}

static void start_connect(const char *ssid, const char *pass)
{
    if (S.scanning) {
        esp_wifi_scan_stop();
        S.scanning = false;
        scan_failed("scan cancelled by CONNECT");
    }
    drop_connection();
    esp_timer_stop(s_timer);

    strlcpy(S.ssid, ssid, sizeof S.ssid);
    strlcpy(S.pass, pass, sizeof S.pass);
    wifi_config_t cfg = { 0 };
    memcpy(cfg.sta.ssid, S.ssid, strlen(S.ssid));
    memcpy(cfg.sta.password, S.pass, strlen(S.pass));
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &cfg);

    S.retries = 0;
    S.last_reason = 0;
    if (e == ESP_OK)
        e = esp_wifi_connect();
    if (e != ESP_OK) { /* refused: the driver would keep (or rejoin) the previous network */
        char essid[100];
        ESP_LOGW(TAG, "connect refused by the driver: %s", esp_err_to_name(e));
        S.last_reason = WIFI_REASON_UNSPECIFIED;
        ctl_send("* CONNECT_FAILED ssid=%s reason=%d why=%s", enc(S.ssid, essid, sizeof essid), S.last_reason,
                 why(S.last_reason));
        S.user_target = false;
        S.save_on_ip = false;
        go_search();
        return;
    }
    set_state(ST_CONNECTING);
}

static void go_search(void)
{
    if (!S.autoc || S.nknown == 0) {
        set_state(ST_IDLE);
        return;
    }
    if (S.st != ST_SEARCHING)
        set_state(ST_SEARCHING);
    if (!S.scanning && !scan_start())
        timer_arm(5 * 1000 * 1000);
}

/* The same actions back both protocols; the caller replies first, so events follow the reply. */

static void act_connect(const char *ssid, const char *pass, bool save)
{
    S.autoc = true;
    S.user_target = true;
    S.save_on_ip = save;
    start_connect(ssid, pass);
}

static void act_disconnect(void)
{
    S.autoc = false;
    esp_timer_stop(s_timer);
    if (S.scanning && !S.scan_for_user && !S.scan_for_sbp) {
        esp_wifi_scan_stop();
        S.scanning = false;
    }
    drop_connection();
    S.last_reason = 0;
    set_state(ST_IDLE);
}

static void act_auto(void)
{
    S.autoc = true;
    if (S.st == ST_IDLE)
        go_search();
}

static bool act_forget(const char *ssid)
{
    if (!known_remove(ssid))
        return false;
    if ((S.st == ST_CONNECTED || S.st == ST_CONNECTING) && !strcmp(S.ssid, ssid)) {
        drop_connection();
        go_search();
    }
    return true;
}

static void act_forget_all(void)
{
    bool current_saved = (S.st == ST_CONNECTED || S.st == ST_CONNECTING) && known_find(S.ssid) >= 0;
    memset(S.known, 0, sizeof S.known);
    S.nknown = 0;
    known_save();
    if (current_saved) {
        drop_connection();
        go_search(); /* nothing left to search for: ends in IDLE */
    }
}

/* Protocol set or channel width changed: they apply to the next association, so associate again
 * (an access point restarts). */
static void radio_reassociate(void)
{
    if (s_ap) {
        wifiap_restart();
        return;
    }
    if (S.st == ST_CONNECTED || S.st == ST_CONNECTING) {
        char ssid[33], pass[65];
        strlcpy(ssid, S.ssid, sizeof ssid);
        strlcpy(pass, S.pass, sizeof pass);
        start_connect(ssid, pass);
    }
}

static void radio_fields(char *out, size_t cap)
{
    static const char *const PS[] = { "none", "min", "max" };
    radio_cfg_t c;
    radio_now_t n;
    radio_get(&c, &n);
    const char *mode = c.proto == RADIO_P_B ? "b" : c.proto == RADIO_P_BG ? "bg" : c.proto == RADIO_P_BGN ? "bgn"
                     : c.proto == RADIO_P_LR ? "lr" : "bgnlr";
    snprintf(out, cap, "power=%u.%02u mode=%s bw=%u ps=%s power_now=%u.%02u phy=%d proto_now=0x%x bw_now=%u",
             c.power / 4, (c.power % 4) * 25, mode, c.bw == 2 ? 40 : 20, PS[c.ps % 3], n.power / 4,
             (n.power % 4) * 25, n.phy == 0xFF ? -1 : n.phy, n.proto, n.bw == 2 ? 40 : 20);
}

/* Returns 0 if a scan is running or was started for the requester, -1 if it cannot start now. */
static int request_scan(void)
{
    if (s_ap)
        return -1; /* an access point has no station to scan with */
    if (S.scanning)
        return 0; /* piggyback on the running scan */
    if (S.st == ST_CONNECTING)
        return -1;
    return scan_start() ? 0 : -1;
}

/* ---- event handlers (driver context -> queue) -------------------------------- */

static void post(msg_t *m)
{
    if (xQueueSend(s_q, m, 0) != pdTRUE && m->buf)
        free(m->buf);
}

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    msg_t m = { 0 };
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        m.type = M_SCAN_DONE;
        m.arg = data ? (int)((wifi_event_sta_scan_done_t *)data)->status : 0; /* 0 = complete */
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        m.type = M_DISCONNECTED;
        m.arg = d->reason;
        memcpy(m.ssid, d->ssid, d->ssid_len < 32 ? d->ssid_len : 32);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        m.type = M_GOT_IP;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        m.type = M_AP_START;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STOP) {
        m.type = M_AP_STOP;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        m.type = M_AP_CLIENT;
        m.arg = CL_JOINED;
        memcpy(m.mac, ((wifi_event_ap_staconnected_t *)data)->mac, 6);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        m.type = M_AP_CLIENT;
        m.arg = CL_LEFT;
        memcpy(m.mac, ((wifi_event_ap_stadisconnected_t *)data)->mac, 6);
    } else if (base == IP_EVENT && id == IP_EVENT_AP_STAIPASSIGNED) {
        ip_event_ap_staipassigned_t *d = data;
        m.type = M_AP_CLIENT;
        m.arg = CL_ADDRESS;
        memcpy(m.mac, d->mac, 6);
        m.ip = d->ip.addr;
    } else {
        return;
    }
    post(&m);
}

static void timer_cb(void *arg)
{
    msg_t m = { .type = M_TIMER };
    post(&m);
}

static void report_cb(void *arg)
{
    msg_t m = { .type = M_REPORT };
    post(&m);
}

static void otatick_cb(void *arg)
{
    msg_t m = { .type = M_OTA_TICK };
    post(&m);
}

/* Room for one more request, the Wi-Fi events keeping theirs. The callers (the UART RX tasks and the network task)
 * outrank the manager, so a burst was queued whole before the manager ran and everything past the queue was lost
 * (0.13 and before). A request that finds the queue full now sleeps, which lets the manager drain it. */
static bool room_for_request(void)
{
    for (int i = 0; uxQueueSpacesAvailable(s_q) <= EVENT_RESERVE; i++) {
        if (i == REQ_WAIT_TICKS) {
            s_req_drops++;
            return false;
        }
        vTaskDelay(1);
    }
    return true;
}

void manager_on_ctl(const uint8_t *line, size_t len)
{
    if (!room_for_request())
        return;
    char *s = malloc(len + 1);
    if (!s)
        return;
    memcpy(s, line, len);
    s[len] = '\0';
    msg_t m = { .type = M_LINE, .buf = s };
    post(&m);
}

void manager_post_sbp(const sbp_frame_t *f, const sbp_chan_t *ch)
{
    if (!room_for_request())
        return;
    sbp_blob_t *b = malloc(sizeof *b + f->len);
    if (!b)
        return;
    b->f = *f;
    b->ch = *ch;
    memcpy(b->payload, f->payload, f->len);
    b->f.payload = b->payload;
    msg_t m = { .type = M_SBP, .buf = b };
    post(&m);
}

/* ---- message processing ------------------------------------------------------ */

static void sbp_send_scan(const wifi_ap_record_t *recs, int n)
{
    if (n == 0) {
        const uint8_t p[2] = { 0, 0 };
        sbpdev_send(SBP_T_CONTENT, 2, SBP_ID_WIFI, p, sizeof p);
        return;
    }
    uint8_t cur[6] = { 0 };
    wifi_ap_record_t ap;
    bool connected = S.st == ST_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    if (connected)
        memcpy(cur, ap.bssid, 6);
    for (int i = 0; i < n; i++) {
        uint8_t p[13 + 32];
        const char *ssid = (const char *)recs[i].ssid;
        size_t sl = strnlen(ssid, 32);
        p[0] = (uint8_t)i;
        p[1] = (uint8_t)n;
        p[2] = (uint8_t)recs[i].rssi;
        p[3] = recs[i].primary;
        p[4] = auth_code(recs[i].authmode);
        p[5] = (uint8_t)((known_find(ssid) >= 0 ? 1 : 0) | (connected && !memcmp(cur, recs[i].bssid, 6) ? 2 : 0));
        memcpy(&p[6], recs[i].bssid, 6);
        p[12] = (uint8_t)sl;
        memcpy(&p[13], ssid, sl);
        sbpdev_send(SBP_T_CONTENT, 2, SBP_ID_WIFI, p, (uint8_t)(13 + sl));
    }
}

static void on_scan_done(int status)
{
    S.scanning = false;
    uint16_t n = MAX_SCAN;
    wifi_ap_record_t *recs = calloc(MAX_SCAN, sizeof *recs);
    if (!recs || esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
        n = 0;
        esp_wifi_clear_ap_list();
    }

    if (status != 0) {
        scan_failed("scan interrupted"); /* e.g. by a reconnect: a partial list would mislead */
    } else if (S.scan_for_user) {
        S.scan_for_user = false;
        for (int i = 0; i < n; i++) {
            char essid[100];
            const char *ssid = (const char *)recs[i].ssid;
            ctl_send("%s NET ssid=%s bssid=" MACSTR " rssi=%d ch=%d auth=%s known=%d", S.scan_tag,
                     enc(ssid, essid, sizeof essid), MAC2STR(recs[i].bssid), recs[i].rssi,
                     recs[i].primary, auth_name(recs[i].authmode), known_find(ssid) >= 0);
        }
        ctl_send("%s OK count=%d", S.scan_tag, n);
    }
    if (status == 0 && S.scan_for_sbp)
        scan_answer(recs, n, true);

    if (S.st == ST_SEARCHING) {
        int best = -1, best_rssi = -1000;
        for (int i = 0; i < n; i++) {
            int k = known_find((const char *)recs[i].ssid);
            if (k >= 0 && recs[i].rssi > best_rssi) {
                best = k;
                best_rssi = recs[i].rssi;
            }
        }
        if (best >= 0) {
            S.user_target = false;
            S.save_on_ip = false;
            start_connect(S.known[best].ssid, S.known[best].pass);
        } else {
            timer_arm(SEARCH_PERIOD_US);
        }
    }
    free(recs);
}

static void on_disconnected(int reason, const char *ssid)
{
    if (S.expect_disc) {
        S.expect_disc = false;
        return;
    }
    if (S.st == ST_IDLE || S.st == ST_SEARCHING)
        return;
    if (ssid[0] && strcmp(ssid, S.ssid) != 0)
        return; /* stale event from a previous target */

    S.last_reason = reason;
    if (S.st == ST_CONNECTED) {
        S.retries = 0;
        if (esp_wifi_connect() != ESP_OK) {
            go_search();
            return;
        }
        set_state(ST_CONNECTING);
        return;
    }
    /* ST_CONNECTING */
    bool give_up = ++S.retries >= CONNECT_RETRIES || (S.user_target && why_code(reason) == WHY_AUTH);
    if (!give_up && esp_wifi_connect() == ESP_OK)
        return;
    char essid[100];
    ctl_send("* CONNECT_FAILED ssid=%s reason=%d why=%s", enc(S.ssid, essid, sizeof essid), reason, why(reason));
    S.user_target = false;
    S.save_on_ip = false;
    go_search(); /* the resulting state report carries reason/why for SBP */
}

static void on_got_ip(void)
{
    if (S.st != ST_CONNECTING && S.st != ST_CONNECTED)
        return;
    radio_post_start(); /* association may have lowered the power limit (AP country info) */
    S.retries = 0;
    S.last_reason = 0;
    if (S.save_on_ip) {
        known_put(S.ssid, S.pass);
        S.save_on_ip = false;
    } else {
        int k = known_find(S.ssid);
        if (k >= 0 && S.known[k].seq != S.seq) { /* only when another network was used last: flash wear */
            S.known[k].seq = ++S.seq;
            known_save();
        }
    }
    S.user_target = false;
    set_state(ST_CONNECTED);
    if (sbpdev_has_listener())
        sbp_send_radio(true); /* the negotiated mode is known only now */
}

/* Every 500 ms: a lost event or a refused driver call must not leave the station stuck for good. */
static void supervise(void)
{
    if (s_ap)
        return;
    int64_t now = esp_timer_get_time();
    if (S.scanning && now - S.scan_since > SCAN_STUCK_US) {
        ESP_LOGW(TAG, "scan never finished: dropped");
        esp_wifi_scan_stop();
        S.scanning = false;
        scan_failed("scan never finished");
        if (S.st == ST_SEARCHING)
            timer_arm(5 * 1000 * 1000);
    }
    wifi_ap_record_t ap;
    if (S.st == ST_CONNECTED) {
        S.ap_lost = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? 0 : S.ap_lost + 1;
        if (S.ap_lost >= AP_LOST_TICKS) { /* the disconnect event never came */
            ESP_LOGW(TAG, "association gone without an event: reconnecting");
            S.retries = 0;
            if (esp_wifi_connect() != ESP_OK)
                go_search();
            else
                set_state(ST_CONNECTING);
        }
    } else if (S.st == ST_CONNECTING && now - S.since > CONNECT_STUCK_US) {
        ESP_LOGW(TAG, "connecting for too long: searching again");
        drop_connection();
        go_search();
    } else if (S.st == ST_SEARCHING && !S.scanning && !esp_timer_is_active(s_timer)) {
        go_search(); /* the retry timer's message was lost */
    }
}

static void on_timer(void)
{
    if (S.st == ST_SEARCHING)
        go_search();
}

static void on_ap_client(int ev, const uint8_t *mac, uint32_t ip)
{
    static const char *const EV[] = { "left", "joined", "address" };
    esp_ip4_addr_t a = { .addr = ip };
    ctl_send("* AP_CLIENT event=%s mac=" MACSTR " ip=" IPSTR, EV[ev], MAC2STR(mac), IP2STR(&a));
    ESP_LOGI(TAG, "client %s " MACSTR " " IPSTR, EV[ev], MAC2STR(mac), IP2STR(&a));
    if (sbpdev_has_listener())
        sbp_send_status(true); /* the client count is in it */
}

/* ---- ID_WIFI (0x57) -------------------------------------------------------------- */

static void sbp_wifi(const sbp_frame_t *f)
{
    uint8_t type = sbp_type(f->mode), ver = sbp_ver(f->mode);
    const uint8_t *p = f->payload;
    bool set = type == SBP_T_SETTING;

    switch (ver) {
    case 0: /* status */
        if (set)
            sbpdev_ack(f, SBP_RESP_ERR_TYPE);
        else
            sbp_send_status(false);
        break;
    case 1: /* link report; SETTING = report period */
        if (set) {
            if (f->len < 2) {
                sbpdev_ack(f, SBP_RESP_ERR_PAYLOAD);
                break;
            }
            uint16_t ms = sbp_get_u16(p);
            if (ms && ms < 100)
                ms = 100;
            if (ms > 60000)
                ms = 60000;
            sbpdev_set_report_ms(ms);
            report_restart();
            sbpdev_ack(f, SBP_RESP_OK);
        }
        sbp_send_link(false);
        break;
    case 2: /* scan (GETTING or SETTING both start one) */
        if (S.scan_for_sbp) {
            if (set)
                sbpdev_ack(f, SBP_RESP_OK); /* results for the earlier request answer this one too */
            break;
        }
        if (request_scan() != 0) {
            sbpdev_ack(f, SBP_RESP_ERR_RUNTIME);
            break;
        }
        S.scan_for_sbp = true;
        S.scan_req = *f;
        S.scan_req.payload = NULL;
        S.scan_req.len = 0;
        sbpdev_channel(&S.scan_ch);
        if (set)
            sbpdev_ack(f, SBP_RESP_OK);
        break;
    case 3: { /* connect: flags, ssid_len, ssid, pass_len, pass */
        if (!set) {
            sbp_send_status(false);
            break;
        }
        if (s_ap) {
            sbpdev_ack(f, SBP_RESP_ERR_RUNTIME); /* no station in the access point role */
            break;
        }
        char ssid[33], pass[65];
        uint8_t sl = f->len >= 2 ? p[1] : 0;
        uint8_t pl = f->len >= 3u + sl ? p[2 + sl] : 0xFF;
        if (sl < 1 || sl > 32 || pl > 64 || f->len != 3u + sl + pl || memchr(p + 2, 0, sl) || memchr(p + 3 + sl, 0, pl)) {
            sbpdev_ack(f, SBP_RESP_ERR_PAYLOAD);
            break;
        }
        memcpy(ssid, p + 2, sl);
        ssid[sl] = '\0';
        memcpy(pass, p + 3 + sl, pl);
        pass[pl] = '\0';
        sbpdev_ack(f, SBP_RESP_OK);
        act_connect(ssid, pass, (p[0] & 1) != 0);
        break;
    }
    case 4: /* saved networks; SETTING = action */
        if (!set) {
            sbp_send_saved();
            break;
        }
        if (f->len < 1) {
            sbpdev_ack(f, SBP_RESP_ERR_PAYLOAD);
            break;
        }
        if (s_ap && p[0] <= 1) {
            sbpdev_ack(f, SBP_RESP_ERR_RUNTIME); /* disconnect/auto need a station */
            break;
        }
        switch (p[0]) {
        case 0:
            sbpdev_ack(f, SBP_RESP_OK);
            act_disconnect();
            break;
        case 1:
            sbpdev_ack(f, SBP_RESP_OK);
            act_auto();
            break;
        case 2: {
            char ssid[33];
            uint8_t sl = f->len >= 2 ? p[1] : 0;
            if (sl < 1 || sl > 32 || f->len != 2u + sl || memchr(p + 2, 0, sl)) {
                sbpdev_ack(f, SBP_RESP_ERR_PAYLOAD);
                break;
            }
            memcpy(ssid, p + 2, sl);
            ssid[sl] = '\0';
            if (known_find(ssid) < 0) {
                sbpdev_ack(f, SBP_RESP_ERR_PAYLOAD);
                break;
            }
            sbpdev_ack(f, SBP_RESP_OK);
            act_forget(ssid);
            break;
        }
        case 3:
            sbpdev_ack(f, SBP_RESP_OK);
            act_forget_all();
            break;
        default:
            sbpdev_ack(f, SBP_RESP_ERR_PAYLOAD);
        }
        break;
    case 5: { /* relay configuration: mode, own address, peer ip, peer port, local port */
        relay_cfg_t c;
        if (set) {
            if (f->len != 10) {
                sbpdev_ack(f, SBP_RESP_ERR_PAYLOAD);
                break;
            }
            c.mode = p[0];
            c.addr = p[1];
            memcpy(c.ip, p + 2, 4);
            c.rport = sbp_get_u16(p + 6);
            c.lport = sbp_get_u16(p + 8);
            if (!relay_cfg_ok(&c)) {
                sbpdev_ack(f, SBP_RESP_ERR_PAYLOAD);
                break;
            }
            sbpdev_ack(f, SBP_RESP_OK); /* from the old address, which the host matches */
            relay_set_cfg(&c);
        }
        relay_get_cfg(&c);
        uint8_t q[10] = { c.mode, c.addr, c.ip[0], c.ip[1], c.ip[2], c.ip[3] };
        sbp_put_u16(q + 6, c.rport);
        sbp_put_u16(q + 8, c.lport);
        sbpdev_send(SBP_T_CONTENT, 5, SBP_ID_WIFI, q, sizeof q);
        break;
    }
    case 6: { /* relay statistics */
        if (set) {
            sbpdev_ack(f, SBP_RESP_ERR_TYPE);
            break;
        }
        relay_stats_t r;
        relay_get_stats(&r);
        uint8_t q[47];
        q[0] = r.state;
        memcpy(q + 1, r.peer_ip, 4);
        sbp_put_u16(q + 5, r.peer_port);
        const uint32_t v[10] = { r.up_frames, r.up_bytes, r.up_packets, r.up_drops, r.down_packets,
                                 r.down_bytes, r.down_frames, r.down_drops, r.local_frames, r.tcp_connects };
        for (int i = 0; i < 10; i++)
            sbp_put_u32(q + 7 + 4 * i, v[i]);
        sbpdev_send(SBP_T_CONTENT, 6, SBP_ID_WIFI, q, sizeof q);
        break;
    }
    case 7: { /* radio: power limit and connection mode */
        if (set) {
            if (f->len != 4) {
                sbpdev_ack(f, SBP_RESP_ERR_PAYLOAD);
                break;
            }
            radio_cfg_t want = { p[0], p[1], p[2], p[3] };
            bool reconnect = false;
            /* LR only is allowed for an access point too (0.12): phones and laptops cannot join it, only
             * Espressif stations with LR, such as another module (a module-to-module link). */
            if (!radio_cfg_ok(&want)) {
                sbpdev_ack(f, SBP_RESP_ERR_PAYLOAD);
                break;
            }
            sbpdev_ack(f, radio_set(&want, &reconnect) ? SBP_RESP_OK : SBP_RESP_ERR_RUNTIME);
            if (reconnect) {
                vTaskDelay(pdMS_TO_TICKS(200)); /* the answer leaves before the radio restarts */
                radio_reassociate();
            }
        }
        sbp_send_radio(false);
        break;
    }
    default:
        sbpdev_ack(f, SBP_RESP_ERR_VERSION);
    }
}

static void handle_sbp(const sbp_frame_t *f)
{
    if (sbpdev_handle(f) == 0)
        return;
    if (f->id == SBP_ID_WIFI)
        sbp_wifi(f);
    else if (f->id == SBP_ID_WIFI_NET)
        netctl_handle(f);
    /* other ids: not ours, ignored like the sonars do */
}

/* ---- text commands ------------------------------------------------------------------ */

static bool arg_dec(const proto_line_t *l, const char *key, char *out, size_t cap, bool required)
{
    const char *v = proto_arg(l, key);
    if (!v) {
        out[0] = '\0';
        return !required;
    }
    int n = pct_decode(v, (uint8_t *)out, cap);
    return n >= 0 && (size_t)n < cap && (int)strlen(out) == n; /* no embedded NULs */
}

static void cmd_ver(const char *tag)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ctl_send("%s OK fw=%s proto=%d mac=" MACSTR " idf=%s reset=%s baud=%lu part=%s ota=%s", tag,
             esp_app_get_description()->version, PROTO_VERSION, MAC2STR(mac), esp_get_idf_version(), reset_name(),
             (unsigned long)link_baud(), ota_running_label(), ota_state_name());
}

static void cmd_stats(const char *tag)
{
    link_stats_t ls;
    bridge_stats_t bs;
    wifistat_t ws;
    link_get_stats(&ls);
    bridge_get_stats(&bs);
    wifistat_get(&ws);
    ctl_send("%s OK link_rx=%lu link_crc=%lu link_disc=%lu link_ovf=%lu link_tx=%lu link_txdrop=%lu "
             "sbp_rx=%lu sbp_ckerr=%lu ip_in=%lu ip_out=%lu ip_bad=%lu dns=%lu dns_servfail=%lu "
             "sta_rx=%lu sta_tx=%lu heap=%lu uptime=%llu",
             tag, (unsigned long)ls.rx_frames, (unsigned long)ls.rx_crc_errors,
             (unsigned long)ls.rx_discarded, (unsigned long)ls.rx_overflows,
             (unsigned long)ls.tx_frames, (unsigned long)ls.tx_dropped,
             (unsigned long)ls.sbp_rx_frames, (unsigned long)ls.sbp_check_errors,
             (unsigned long)bs.from_host, (unsigned long)bs.to_host, (unsigned long)bs.from_host_bad,
             (unsigned long)bs.dns_queries, (unsigned long)bs.dns_servfail,
             (unsigned long)ws.rx_total, (unsigned long)ws.tx_total,
             (unsigned long)esp_get_free_heap_size(), (unsigned long long)(esp_timer_get_time() / 1000000));
}

static void handle_line(char *line)
{
    proto_line_t l;
    if (proto_parse(line, &l) != 0) {
        ctl_send("? ERR BAD_LINE");
        return;
    }
    char tag[16];
    strlcpy(tag, l.tag, sizeof tag);
    const char *c = l.cmd;

    if (!strcmp(c, "VER")) {
        cmd_ver(tag);
    } else if (!strcmp(c, "STATUS")) {
        char f[380];
        status_fields(f, sizeof f);
        ctl_send("%s OK %s", tag, f);
    } else if (!strcmp(c, "SCAN")) {
        if (S.scan_for_user) {
            ctl_send("%s ERR BUSY scan already requested", tag);
        } else if (request_scan() != 0) {
            ctl_send("%s ERR BUSY connecting or scan did not start", tag);
        } else {
            S.scan_for_user = true;
            strlcpy(S.scan_tag, tag, sizeof S.scan_tag);
        }
    } else if (s_ap && (!strcmp(c, "CONNECT") || !strcmp(c, "DISCONNECT") || !strcmp(c, "AUTO"))) {
        ctl_send("%s ERR ROLE access point: no station", tag);
    } else if (!strcmp(c, "CONNECT")) {
        char ssid[33], pass[65], save[4];
        if (!arg_dec(&l, "ssid", ssid, sizeof ssid, true) || !ssid[0] ||
            !arg_dec(&l, "pass", pass, sizeof pass, false) ||
            !arg_dec(&l, "save", save, sizeof save, false)) {
            ctl_send("%s ERR BAD_ARGS need ssid=<1..32 bytes> [pass=<0..64 bytes>] [save=0|1]", tag);
            return;
        }
        ctl_send("%s OK", tag);
        act_connect(ssid, pass, strcmp(save, "0") != 0);
    } else if (!strcmp(c, "DISCONNECT")) {
        ctl_send("%s OK", tag);
        act_disconnect();
    } else if (!strcmp(c, "AUTO")) {
        ctl_send("%s OK", tag);
        act_auto();
    } else if (!strcmp(c, "LIST")) {
        for (int i = 0; i < S.nknown; i++) {
            char essid[100];
            ctl_send("%s KNOWN ssid=%s", tag, enc(S.known[i].ssid, essid, sizeof essid));
        }
        ctl_send("%s OK count=%d", tag, S.nknown);
    } else if (!strcmp(c, "FORGET")) {
        char ssid[33];
        if (!arg_dec(&l, "ssid", ssid, sizeof ssid, true)) {
            ctl_send("%s ERR BAD_ARGS need ssid=", tag);
        } else if (known_find(ssid) < 0) {
            ctl_send("%s ERR NOT_FOUND", tag);
        } else {
            ctl_send("%s OK", tag);
            act_forget(ssid);
        }
    } else if (!strcmp(c, "STATS")) {
        cmd_stats(tag);
    } else if (!strcmp(c, "BAUD")) {
        char rate[12];
        unsigned long b = 0;
        if (!arg_dec(&l, "rate", rate, sizeof rate, true) || sscanf(rate, "%lu", &b) != 1 ||
            b < LINK_BAUD_MIN || b > LINK_BAUD_MAX) {
            ctl_send("%s ERR BAD_ARGS need rate=%u..%u", tag, LINK_BAUD_MIN, LINK_BAUD_MAX);
            return;
        }
        ctl_send("%s OK", tag);    /* queued before the switch: leaves at the old rate */
        link_set_baud((uint32_t)b);
    } else if (!strcmp(c, "PROTO")) {
        char p[8];
        if (!arg_dec(&l, "link", p, sizeof p, true) || (strcmp(p, "sbp") && strcmp(p, "slip"))) {
            ctl_send("%s ERR BAD_ARGS need link=sbp|slip", tag);
            return;
        }
        ctl_send("%s OK", tag);
        if (!strcmp(p, "sbp"))
            link_set_proto(LINK_PROTO_SBP);
    } else if (!strcmp(c, "RADIO")) {
        radio_cfg_t rc;
        radio_now_t rn;
        radio_get(&rc, &rn);
        char v[12];
        if (arg_dec(&l, "power", v, sizeof v, false) && v[0])
            rc.power = (uint8_t)(atof(v) * 4 + 0.5);
        if (arg_dec(&l, "mode", v, sizeof v, false) && v[0])
            rc.proto = !strcmp(v, "b") ? RADIO_P_B : !strcmp(v, "bg") ? RADIO_P_BG : !strcmp(v, "bgn") ? RADIO_P_BGN
                     : !strcmp(v, "lr") ? RADIO_P_LR : !strcmp(v, "bgnlr") ? RADIO_P_BGN_LR : 0;
        if (arg_dec(&l, "bw", v, sizeof v, false) && v[0])
            rc.bw = !strcmp(v, "40") ? 2 : !strcmp(v, "20") ? 1 : 0;
        if (arg_dec(&l, "ps", v, sizeof v, false) && v[0])
            rc.ps = !strcmp(v, "none") ? 0 : !strcmp(v, "min") ? 1 : !strcmp(v, "max") ? 2 : 3;
        if (l.argc > 0) {
            bool reconnect = false;
            if (!radio_cfg_ok(&rc)) {
                ctl_send("%s ERR BAD_ARGS power=2..21 dBm mode=b|bg|bgn|lr|bgnlr bw=20|40 (40 needs n) ps=none|min|max", tag);
                return;
            }
            if (!radio_set(&rc, &reconnect)) {
                ctl_send("%s ERR FAIL driver refused", tag);
                return;
            }
            char f2[200];
            radio_fields(f2, sizeof f2);
            ctl_send("%s OK %s", tag, f2);
            if (reconnect)
                radio_reassociate();
            return;
        }
        char f2[200];
        radio_fields(f2, sizeof f2);
        ctl_send("%s OK %s", tag, f2);
    } else if (!strcmp(c, "RELAY")) {
        relay_cfg_t rc;
        relay_get_cfg(&rc);
        char v[20] = "";
        bool ok = true; /* a misspelt or undecodable value is an error, never silently "off" */
        if (proto_arg(&l, "mode")) {
            ok = arg_dec(&l, "mode", v, sizeof v, true) && (!strcmp(v, "off") || !strcmp(v, "udp") || !strcmp(v, "tcp"));
            if (ok)
                rc.mode = !strcmp(v, "udp") ? RELAY_UDP : !strcmp(v, "tcp") ? RELAY_TCP : RELAY_OFF;
        }
        if (ok && (ok = arg_dec(&l, "addr", v, sizeof v, false)) && v[0])
            rc.addr = (uint8_t)atoi(v);
        if (ok && (ok = arg_dec(&l, "ip", v, sizeof v, false)) && v[0]) {
            unsigned a, b, cc, d;
            ok = sscanf(v, "%u.%u.%u.%u", &a, &b, &cc, &d) == 4 && a < 256 && b < 256 && cc < 256 && d < 256;
            if (ok) {
                rc.ip[0] = (uint8_t)a; rc.ip[1] = (uint8_t)b; rc.ip[2] = (uint8_t)cc; rc.ip[3] = (uint8_t)d;
            }
        }
        if (ok && (ok = arg_dec(&l, "port", v, sizeof v, false)) && v[0])
            rc.rport = (uint16_t)atoi(v);
        if (ok && (ok = arg_dec(&l, "lport", v, sizeof v, false)) && v[0])
            rc.lport = (uint16_t)atoi(v);
        if (!ok || (l.argc > 0 && !relay_cfg_ok(&rc))) {
            ctl_send("%s ERR BAD_ARGS mode=off|udp|tcp addr= ip= port= lport= (tcp needs ip, relaying needs port)", tag);
            return;
        }
        relay_stats_t r;
        relay_get_stats(&r);
        ctl_send("%s OK mode=%s addr=%u ip=%u.%u.%u.%u port=%u lport=%u state=%u peer=%u.%u.%u.%u:%u "
                 "up_frames=%lu up_packets=%lu up_drops=%lu down_packets=%lu down_frames=%lu down_drops=%lu local=%lu",
                 tag, rc.mode == RELAY_UDP ? "udp" : rc.mode == RELAY_TCP ? "tcp" : "off", rc.addr, rc.ip[0], rc.ip[1],
                 rc.ip[2], rc.ip[3], rc.rport, rc.lport, r.state, r.peer_ip[0], r.peer_ip[1], r.peer_ip[2],
                 r.peer_ip[3], r.peer_port, (unsigned long)r.up_frames, (unsigned long)r.up_packets,
                 (unsigned long)r.up_drops, (unsigned long)r.down_packets, (unsigned long)r.down_frames,
                 (unsigned long)r.down_drops, (unsigned long)r.local_frames);
        if (l.argc > 0)
            relay_set_cfg(&rc); /* after the reply: an active relay locks the port to SBP */
    } else if (!strcmp(c, "REBOOT")) {
        ctl_send("%s OK", tag);
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    } else {
        ctl_send("%s ERR UNKNOWN_CMD", tag);
    }
}

static void manager_task(void *arg)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ctl_send("* HELLO fw=%s proto=%d mac=" MACSTR " reset=%s", esp_app_get_description()->version, PROTO_VERSION,
             MAC2STR(mac), reset_name());

    if (!s_ap) { /* an access point reports itself on WIFI_EVENT_AP_START */
        S.autoc = S.nknown > 0;
        go_search();
    }

    msg_t m;
    for (;;) {
        if (xQueueReceive(s_q, &m, portMAX_DELAY) != pdTRUE)
            continue;
        switch (m.type) {
        case M_LINE:
            ota_note_host_frame();
            handle_line(m.buf);
            free(m.buf);
            break;
        case M_SBP: {
            sbp_blob_t *b = m.buf;
            ota_note_host_frame();
            int port = sbpdev_chan_port(&b->ch);
            if (port >= 0)
                portinfo_module_rx(port);
            sbpdev_begin_request(&b->ch);
            handle_sbp(&b->f);
            sbpdev_end_request();
            free(b);
            break;
        }
        case M_OTA_TICK:
            ota_tick();
            supervise();
            break;
        case M_SCAN_DONE:    on_scan_done(m.arg); break;
        case M_DISCONNECTED: on_disconnected(m.arg, m.ssid); break;
        case M_GOT_IP:       on_got_ip(); break;
        case M_TIMER:        on_timer(); break;
        case M_AP_START:
            radio_post_start(); /* power limit again after every (re)start */
            set_state(ST_AP);
            if (sbpdev_has_listener())
                sbp_send_radio(true);
            break;
        case M_AP_STOP:      set_state(ST_IDLE); break;
        case M_AP_CLIENT:    on_ap_client(m.arg, m.mac, m.ip); break;
        case M_REPORT:
            if (sbpdev_has_listener())
                sbp_send_link(true);
            break;
        }
    }
}

void manager_init(esp_netif_t *net)
{
    s_net = net;
    s_ap = netcfg_role() == ROLE_AP;
    s_q = xQueueCreate(QUEUE_LEN, sizeof(msg_t));
    configASSERT(s_q);
    known_load();
    const esp_timer_create_args_t ta = { .callback = timer_cb, .name = "mgr" };
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_timer));
    const esp_timer_create_args_t tr = { .callback = report_cb, .name = "wifi_rep" };
    ESP_ERROR_CHECK(esp_timer_create(&tr, &s_report));
    const esp_timer_create_args_t to = { .callback = otatick_cb, .name = "ota_tick" };
    ESP_ERROR_CHECK(esp_timer_create(&to, &s_otatick));
    /* Registered before esp_wifi_start(): WIFI_EVENT_AP_START follows it at once. Events wait in the
     * queue until the task runs. */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_evt, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, wifi_evt, NULL));
}

void manager_start(void)
{
    xTaskCreate(manager_task, "manager", 6144, NULL, 6, NULL);
    report_restart();
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_otatick, 500 * 1000));
}
