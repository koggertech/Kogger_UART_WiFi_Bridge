#include "radio.h"

#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs.h"

static const char *TAG = "radio";

/* Defaults: full power, b/g/n, 20 MHz (less interference than 40 on a busy band), no power save
 * (mains powered, lowest latency). Full power is 20 dBm: the driver takes nothing above (esp_wifi.h
 * esp_wifi_set_max_tx_power), so a larger request is stored as 80 and "set" equals "in effect". */
#define POWER_MAX 80
static radio_cfg_t s_cfg = { .power = POWER_MAX, .proto = RADIO_P_BGN, .bw = 1, .ps = 0 };
static wifi_interface_t s_if = WIFI_IF_STA;

bool radio_cfg_ok(const radio_cfg_t *c)
{
    if (c->power < 8 || c->power > 84)
        return false;
    if (c->proto != RADIO_P_B && c->proto != RADIO_P_BG && c->proto != RADIO_P_BGN && c->proto != RADIO_P_LR &&
        c->proto != RADIO_P_BGN_LR)
        return false;
    if (c->bw != 1 && c->bw != 2)
        return false;
    if (c->bw == 2 && !(c->proto & WIFI_PROTOCOL_11N))
        return false; /* 40 MHz exists only in 11n */
    return c->ps <= 2;
}

static void load(void)
{
    nvs_handle_t h;
    radio_cfg_t c;
    size_t sz = sizeof c;
    if (nvs_open("wb", NVS_READONLY, &h) != ESP_OK)
        return;
    if (nvs_get_blob(h, "radio", &c, &sz) == ESP_OK && sz == sizeof c && radio_cfg_ok(&c)) {
        s_cfg = c;
        if (s_cfg.power > POWER_MAX)
            s_cfg.power = POWER_MAX;
    }
    nvs_close(h);
}

static bool save(const radio_cfg_t *c)
{
    nvs_handle_t h;
    if (nvs_open("wb", NVS_READWRITE, &h) != ESP_OK)
        return false;
    bool ok = nvs_set_blob(h, "radio", c, sizeof *c) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

static esp_err_t set_mode(uint8_t proto, uint8_t bw)
{
    esp_err_t e = esp_wifi_set_protocol(s_if, proto);
    if (e == ESP_OK)
        e = esp_wifi_set_bandwidth(s_if, (wifi_bandwidth_t)bw);
    return e;
}

void radio_init(bool ap)
{
    s_if = ap ? WIFI_IF_AP : WIFI_IF_STA;
    load();
    esp_err_t e = set_mode(s_cfg.proto, s_cfg.bw);
    if (e != ESP_OK)
        ESP_LOGW(TAG, "protocol 0x%x / width %u refused: %s", s_cfg.proto, s_cfg.bw, esp_err_to_name(e));
}

void radio_post_start(void)
{
    if (s_if == WIFI_IF_STA)
        esp_wifi_set_ps((wifi_ps_type_t)s_cfg.ps);
    /* The AP's country information can lower the limit on association, so this runs after every
     * connection too; the driver never exceeds what the country allows. */
    esp_err_t e = esp_wifi_set_max_tx_power((int8_t)s_cfg.power);
    if (e != ESP_OK)
        ESP_LOGW(TAG, "tx power %u refused: %s", s_cfg.power, esp_err_to_name(e));
    /* A protocol/width call refused before the start is only logged there: check what the driver
     * really holds and set it again (it applies from the next association on). */
    uint8_t proto = 0;
    wifi_bandwidth_t bw = WIFI_BW_HT20;
    if (esp_wifi_get_protocol(s_if, &proto) == ESP_OK && esp_wifi_get_bandwidth(s_if, &bw) == ESP_OK &&
        (proto != s_cfg.proto || bw != (wifi_bandwidth_t)s_cfg.bw)) {
        ESP_LOGW(TAG, "driver holds protocol 0x%x / width %d instead of 0x%x / %u: set again", proto, (int)bw,
                 s_cfg.proto, s_cfg.bw);
        set_mode(s_cfg.proto, s_cfg.bw);
    }
}

bool radio_set(const radio_cfg_t *req, bool *reconnect)
{
    *reconnect = false;
    if (!radio_cfg_ok(req))
        return false;
    radio_cfg_t cc = *req, *c = &cc;
    if (c->power > POWER_MAX)
        c->power = POWER_MAX;
    bool mode_changed = c->proto != s_cfg.proto || c->bw != s_cfg.bw;
    if (mode_changed) {
        esp_err_t e = set_mode(c->proto, c->bw);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "mode change refused: %s", esp_err_to_name(e));
            set_mode(s_cfg.proto, s_cfg.bw); /* back to what worked */
            return false;
        }
    }
    s_cfg = *c;
    radio_post_start();
    if (!save(c))
        ESP_LOGW(TAG, "settings not saved");
    *reconnect = mode_changed;
    ESP_LOGI(TAG, "power %u/4 dBm, protocol 0x%x, width %u, ps %u", c->power, c->proto, c->bw, c->ps);
    return true;
}

void radio_get(radio_cfg_t *c, radio_now_t *now)
{
    *c = s_cfg;
    int8_t p = 0;
    uint8_t proto = 0;
    wifi_bandwidth_t bw = WIFI_BW_HT20;
    wifi_phy_mode_t phy;
    esp_wifi_get_max_tx_power(&p);
    esp_wifi_get_protocol(s_if, &proto);
    esp_wifi_get_bandwidth(s_if, &bw);
    now->power = (uint8_t)p;
    now->proto = proto;
    now->bw = (uint8_t)bw;
    now->phy = s_if == WIFI_IF_STA && esp_wifi_sta_get_negotiated_phymode(&phy) == ESP_OK ? (uint8_t)phy : 0xFF;
}
