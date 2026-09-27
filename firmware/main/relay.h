/*
 * relay.h - bridges each physical UART line to its own UDP (or TCP) socket over Wi-Fi, in either
 * role (station or access point). The UART stream is cut into whole frames (kframe) and packed into
 * packets of at most 512 bytes (kpack); the network stream is cut into whole frames again before it
 * goes to the UART, so frames of the module itself never land inside a relayed frame.
 * Frames addressed to the module's own SBP address are handled locally - from a UART line or from
 * the network (then the answer goes back to the sender) - everything else is relayed.
 * Buffers, back-pressure, drop rules: docs/RELAY.md; lines and roles: docs/NETWORK.md.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_netif.h"
#include "netcfg.h"

/* ---- ID_WIFI v5 view of line 0 (kept from 0.7 for existing hosts) ---------------------------- */

typedef enum { RELAY_OFF = 0, RELAY_UDP = 1, RELAY_TCP = 2 } relay_mode_t;

typedef struct {
    uint8_t  mode;     /**< relay_mode_t */
    uint8_t  addr;     /**< the module's own SBP address while bridging */
    uint8_t  ip[4];    /**< peer; 0.0.0.0 = answer the senders, 255.255.255.255 = subnet broadcast */
    uint16_t rport;
    uint16_t lport;    /**< 0 = rport */
} relay_cfg_t;

typedef enum { RS_OFF = 0, RS_NO_WIFI, RS_NO_PEER, RS_READY } relay_state_t;

typedef struct {
    uint8_t  state;              /**< relay_state_t */
    uint8_t  peer_ip[4];         /**< fixed peer, or the most recent sender */
    uint16_t peer_port;
    uint8_t  npeers;             /**< senders currently answered (DEST_SENDERS) */
    uint32_t up_frames;          /**< whole frames UART -> network */
    uint32_t up_bytes;
    uint32_t up_packets;
    uint32_t up_drops;           /**< packets dropped: pool full, nobody to send to, send error */
    uint32_t down_packets;
    uint32_t down_bytes;
    uint32_t down_frames;        /**< whole frames network -> UART */
    uint32_t down_drops;         /**< units dropped: UART ring full */
    uint32_t local_frames;       /**< frames for the module itself (from this line or its socket) */
    uint32_t tcp_connects;
} relay_stats_t;

/** Buffers + saved configuration. Before link_start(). */
void relay_init(void);

/** Line 1 buffers, own address while bridging, SBP lock of the host port, network tasks.
 *  After uline_start(), relay_set_netif() and sbpdev_load(), before link_start(). */
void relay_start(void);

/** The interface whose subnet broadcast DEST_BROADCAST uses (station or AP netif). */
void relay_set_netif(esp_netif_t *netif);

bool relay_line_active(int line);
bool relay_active(void);                      /**< line 0 bridges (link.c hands it the UART stream) */

bool relay_line_set(int line, const line_cfg_t *c);   /**< save + apply (ID_WIFI_NET v3) */
void relay_line_stats(int line, relay_stats_t *s);

/** Save the module's own address while bridging and apply it (ID_WIFI_NET v6); 0 is refused. */
bool relay_set_addr(uint8_t addr);

void relay_get_cfg(relay_cfg_t *c);
bool relay_cfg_ok(const relay_cfg_t *c);
bool relay_set_cfg(const relay_cfg_t *c);
void relay_get_stats(relay_stats_t *s);       /**< line 0 */

/** Station has an IP address / access point is running. */
void relay_wifi(bool up);

/** UART RX tasks: bytes of a line, and "the line went quiet" (long = tens of milliseconds). */
void relay_line_bytes(int line, const uint8_t *d, size_t n);
void relay_line_idle(int line, bool long_idle);
void relay_uart_bytes(const uint8_t *d, size_t n);    /**< line 0 */
void relay_uart_idle(bool long_idle);                 /**< line 0 */

/** Send one frame of the module itself to a network peer of a line (answer to a frame from it).
 *  ip and port in network byte order. */
void relay_send_to(int line, uint32_t ip, uint16_t port, const uint8_t *d, size_t n);
