/*
 * manager.h - Wi-Fi control (station: saved networks in NVS, search, reconnect; access point:
 * start, clients) and both control protocols: text lines over SLIP (docs/PROTOCOL.md) and
 * ID_WIFI / ID_WIFI_NET over Kogger SBP (docs/SBP_WIFI.md).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_netif.h"
#include "sbp.h"
#include "sbpdev.h"

/** Create the message queue, load saved networks and register the Wi-Fi/IP event handlers.
 *  Call before link_start() and esp_wifi_start(): frames and events may arrive at once. */
void manager_init(esp_netif_t *net);

/** Start the manager task (sends * HELLO) and the ID_WIFI v1 report timer. After esp_wifi_start(),
 *  link_start() and sbpdev_init(). */
void manager_start(void);

/** Restart the ID_WIFI v1 report timer with the current period (after ID_FLASH v1 reloads it). */
void manager_report_restart(void);

/** Link RX handler for FRAME_T_CTL. */
void manager_on_ctl(const uint8_t *line, size_t len);

/** Queue an SBP frame addressed to us with the channel it came from (called from the link, line or
 *  network RX tasks via sbpdev_on_frame_ch; copies it). */
void manager_post_sbp(const sbp_frame_t *f, const sbp_chan_t *ch);
