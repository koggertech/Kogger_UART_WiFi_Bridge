/*
 * bridge.h - point-to-point lwIP interface towards the host, NAPT into the Wi-Fi STA,
 * and a DNS relay on the bridge address.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_netif.h"

typedef struct {
    uint32_t to_host;        /**< IPv4 packets sent to the host */
    uint32_t from_host;      /**< IPv4 packets accepted from the host */
    uint32_t from_host_bad;  /**< dropped: not IPv4, too short, or no pbuf */
    uint32_t dns_queries;
    uint32_t dns_servfail;   /**< answered locally because there is no upstream */
} bridge_stats_t;

/** Add the interface and enable NAPT on it. net (station) is used to find the upstream DNS server;
 *  an access point has none (queries are answered SERVFAIL). */
void bridge_start(esp_netif_t *net, bool ap);

/** Link RX handler for FRAME_T_IP. */
void bridge_input(const uint8_t *pkt, size_t len);

void bridge_get_stats(bridge_stats_t *out);
