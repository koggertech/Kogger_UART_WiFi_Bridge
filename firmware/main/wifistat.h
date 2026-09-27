/*
 * wifistat.h - traffic of the Wi-Fi interface (station or access point): byte totals and bytes per
 * second.
 *
 * Counts every L2 frame the netif receives or sends (payload of the 802.11 data frames as lwIP sees
 * them: Ethernet header + IP packet), whatever the data path above it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_netif.h"

typedef struct {
    uint32_t rx_total;   /**< bytes received since boot (wraps at 2^32) */
    uint32_t tx_total;   /**< bytes sent since boot (wraps at 2^32) */
    uint32_t rx_bps;     /**< bytes received during the last full second */
    uint32_t tx_bps;     /**< bytes sent during the last full second */
} wifistat_t;

/** Start counting on the interface; hooks are (re)installed on every IP_EVENT_STA_GOT_IP (station)
 *  or WIFI_EVENT_AP_START (access point). */
void wifistat_start(esp_netif_t *netif, bool ap);

void wifistat_get(wifistat_t *out);
