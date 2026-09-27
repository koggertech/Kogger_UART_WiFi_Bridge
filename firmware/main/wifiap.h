/*
 * wifiap.h - the access point role: Wi-Fi AP settings, the AP's address and its DHCP server
 * (settings from netcfg), clients. docs/NETWORK.md.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_netif.h"
#include "netcfg.h"

/** Configure the AP before esp_wifi_start(): Wi-Fi settings, address, DHCP server. Falls back to the
 *  built-in defaults when the driver refuses the saved settings; false if it refuses those too - the
 *  radio must then not start (the driver's own configuration is an open network). */
bool wifiap_setup(esp_netif_t *ap);

/** Change the Wi-Fi settings while running: the AP restarts, connected clients drop and rejoin. */
bool wifiap_apply_ap(const ap_cfg_t *c);

/** Restart the AP so protocol/width changes take effect. */
void wifiap_restart(void);

/** Change the address / DHCP server while running; clients are dropped so they ask DHCP again. */
bool wifiap_apply_ip(const ip_cfg_t *c);

typedef struct {
    uint8_t mac[6];
    uint8_t ip[4];    /**< 0.0.0.0 until DHCP has given one */
    int8_t  rssi;
} ap_client_t;

/** Connected stations, up to max; returns how many. */
int wifiap_clients(ap_client_t *out, int max);

esp_netif_t *wifiap_netif(void);
