#include "wifiap.h"

#include <string.h>

#include "dhcpserver/dhcpserver.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"

static const char *TAG = "wifiap";

static esp_netif_t *s_ap;
static bool s_dhcp;               /* the DHCP server is meant to run */

esp_netif_t *wifiap_netif(void)
{
    return s_ap;
}

static esp_err_t set_wifi(const ap_cfg_t *c)
{
    wifi_config_t w = { 0 };
    size_t sl = strnlen(c->ssid, 32), pl = strnlen(c->pass, 64);
    memcpy(w.ap.ssid, c->ssid, sl);
    w.ap.ssid_len = (uint8_t)sl;
    memcpy(w.ap.password, c->pass, pl);
    w.ap.channel = c->channel;
    w.ap.max_connection = c->max_clients;
    w.ap.ssid_hidden = c->hidden;
    w.ap.authmode = c->auth == 0 ? WIFI_AUTH_OPEN : c->auth == 1 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_WPA2_WPA3_PSK;
    w.ap.pmf_cfg.capable = true;
    w.ap.pmf_cfg.required = false;
    if (c->auth == 2)
        w.ap.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    return esp_wifi_set_config(WIFI_IF_AP, &w);
}

static esp_ip4_addr_t ip4(const uint8_t *a)
{
    esp_ip4_addr_t r;
    memcpy(&r.addr, a, 4); /* a.b.c.d in memory order = network order */
    return r;
}

static esp_err_t set_ip(const ip_cfg_t *c)
{
    s_dhcp = c->dhcp;
    esp_netif_dhcps_stop(s_ap); /* options can only be changed while the server is stopped */
    esp_netif_ip_info_t ii = { .ip = ip4(c->ip), .netmask = ip4(c->mask), .gw = ip4(c->ip) };
    esp_err_t e = esp_netif_set_ip_info(s_ap, &ii);
    if (e != ESP_OK || !c->dhcp)
        return e;
    dhcps_lease_t lease = { .enable = true };
    lease.start_ip.addr = ip4(c->pool_start).addr;
    lease.end_ip.addr = ip4(c->pool_end).addr;
    dhcps_time_t t = c->lease_min; /* CONFIG_LWIP_DHCPS_LEASE_UNIT = 60 s by default: minutes */
    dhcps_offer_t gw = (c->offer & IPCFG_OFFER_GW) ? OFFER_ROUTER : 0;
    dhcps_offer_t dns = (c->offer & IPCFG_OFFER_DNS) ? OFFER_DNS : 0;
    if (dns) { /* the address offered as DNS is the AP's own */
        esp_netif_dns_info_t d = { .ip.type = ESP_IPADDR_TYPE_V4 };
        d.ip.u_addr.ip4 = ip4(c->ip);
        esp_netif_set_dns_info(s_ap, ESP_NETIF_DNS_MAIN, &d);
    }
    if ((e = esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_REQUESTED_IP_ADDRESS, &lease, sizeof lease)) != ESP_OK ||
        (e = esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_IP_ADDRESS_LEASE_TIME, &t, sizeof t)) != ESP_OK ||
        (e = esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_ROUTER_SOLICITATION_ADDRESS, &gw, sizeof gw)) != ESP_OK ||
        (e = esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &dns, sizeof dns)) != ESP_OK)
        return e;
    return esp_netif_dhcps_start(s_ap);
}

/* esp_netif restarts the DHCP server on every AP start when it was never running (it sets the state
 * back to INIT on stop), so a server switched off from boot would come back after an AP restart with
 * default options. This runs after esp_netif's own handler and stops it again. */
static void on_ap_start(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (!s_dhcp)
        esp_netif_dhcps_stop(s_ap);
}

bool wifiap_setup(esp_netif_t *ap)
{
    s_ap = ap;
    ap_cfg_t a;
    ip_cfg_t i;
    netcfg_ap(&a);
    netcfg_ip(&i);
    esp_err_t e = set_wifi(&a);
    if (e != ESP_OK) {
        /* Never leave the driver's own configuration in place: that is an open network. */
        ESP_LOGE(TAG, "AP settings refused (%s): built-in defaults", esp_err_to_name(e));
        netcfg_ap_default(&a);
        e = set_wifi(&a);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "defaults refused too (%s): the access point stays off", esp_err_to_name(e));
            return false;
        }
    }
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START, on_ap_start, NULL);
    e = set_ip(&i);
    if (e != ESP_OK)
        ESP_LOGE(TAG, "address/DHCP refused: %s", esp_err_to_name(e));
    ESP_LOGI(TAG, "AP \"%s\" ch %u, %u.%u.%u.%u/%u.%u.%u.%u, DHCP %s %u.%u.%u.%u-%u.%u.%u.%u", a.ssid, a.channel,
             i.ip[0], i.ip[1], i.ip[2], i.ip[3], i.mask[0], i.mask[1], i.mask[2], i.mask[3], i.dhcp ? "on" : "off",
             i.pool_start[0], i.pool_start[1], i.pool_start[2], i.pool_start[3], i.pool_end[0], i.pool_end[1],
             i.pool_end[2], i.pool_end[3]);
    return true;
}

/* The IDF does not document how a running AP takes new settings, so it is stopped and started:
 * clients drop and rejoin, the relay closes and reopens its sockets on the AP events. */
bool wifiap_apply_ap(const ap_cfg_t *c)
{
    esp_wifi_stop();
    esp_err_t e = set_wifi(c);
    if (e != ESP_OK)
        ESP_LOGW(TAG, "AP settings refused: %s", esp_err_to_name(e));
    esp_wifi_start();
    return e == ESP_OK;
}

void wifiap_restart(void)
{
    esp_wifi_stop();
    esp_wifi_start();
}

bool wifiap_apply_ip(const ip_cfg_t *c)
{
    esp_err_t e = set_ip(c);
    if (e != ESP_OK)
        ESP_LOGW(TAG, "address/DHCP refused: %s", esp_err_to_name(e));
    esp_wifi_deauth_sta(0); /* everybody rejoins and asks DHCP again instead of keeping an old lease */
    return e == ESP_OK;
}

int wifiap_clients(ap_client_t *out, int max)
{
    wifi_sta_list_t list;
    if (!s_ap || esp_wifi_ap_get_sta_list(&list) != ESP_OK)
        return 0;
    int n = list.num < max ? list.num : max;
    esp_netif_pair_mac_ip_t pairs[ESP_WIFI_MAX_CONN_NUM];
    for (int i = 0; i < n; i++) {
        memcpy(pairs[i].mac, list.sta[i].mac, 6);
        pairs[i].ip.addr = 0;
    }
    if (n)
        esp_netif_dhcps_get_clients_by_mac(s_ap, n, pairs);
    for (int i = 0; i < n; i++) {
        memcpy(out[i].mac, list.sta[i].mac, 6);
        memcpy(out[i].ip, &pairs[i].ip.addr, 4);
        out[i].rssi = list.sta[i].rssi;
    }
    return n;
}
