/*
 * sbpdev.h - the module as a Kogger SBP device: identity, UART/address settings, acknowledgements.
 * Wi-Fi itself (ID_WIFI) is handled by manager.c. Contract: docs/SBP_WIFI.md.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sbp.h"

#define SBP_ID_UART     0x18
#define SBP_ID_VERSION  0x20
#define SBP_ID_MARK     0x21
#define SBP_ID_FLASH    0x23
#define SBP_ID_BOOT     0x24
#define SBP_ID_WIFI     0x57   /**< chosen 2026-09-25 from the free pool, see docs/SBP_WIFI.md */
#define SBP_ID_WIFI_NET 0x58   /**< role, access point, DHCP, lines; chosen 2026-09-26, docs/SBP_WIFI.md */

/** Board id reported in ID_VERSION (KoggerApp BoardVersion), chosen from the free pool. */
#define SBP_BOARD_WIFI  87

/** Broadcast routes: a GETTING ID_VERSION to either is answered from the module's own address and still
 *  relayed (discovery, docs/SBP_WIFI.md). 0 is also where devices sit by default. */
#define SBP_ROUTE_BCAST0 0
#define SBP_ROUTE_BCAST  255

static inline bool sbp_is_discovery(uint8_t route, uint8_t mode, uint8_t id)
{
    return (route == SBP_ROUTE_BCAST0 || route == SBP_ROUTE_BCAST) && sbp_type(mode) == SBP_T_GETTING &&
           id == SBP_ID_VERSION;
}

/** Where a frame for the module came from: X1 (line 0), X2 (line 1) or a network peer of a line. */
typedef enum { CH_NONE = 0, CH_LINK = 1, CH_LINE1 = 2, CH_NET = 3 } sbp_chan_kind_t;

typedef struct {
    uint8_t  kind;   /**< sbp_chan_kind_t: host link (line 0), UART1 (line 1), network peer of a line */
    uint8_t  line;   /**< CH_NET: the line whose socket received the frame */
    uint16_t port;   /**< CH_NET: sender, network byte order */
    uint32_t ip;     /**< CH_NET: sender, network byte order */
} sbp_chan_t;

/** Load saved settings (report period, rates) and the one-shot resume record; sets the own address.
 *  No link calls: after netcfg_load(), before relay_start() and link_start(). */
void sbpdev_load(void);

/** Apply the loaded rates (resume record first, else the saved one). After link_start(). */
void sbpdev_start(void);

/** RX side (relay.c, any line or the network): takes SETTING/GETTING for the own address and discovery,
 *  locks X1 to SBP when it came from there, queues it for the manager. */
void sbpdev_on_frame_ch(const sbp_frame_t *f, const sbp_chan_t *ch);

/** Manager task, around one request: its answers go to its channel, and the channel is subscribed to
 *  the unsolicited frames (state reports, update progress) for SUB_TTL of its last request. */
void sbpdev_begin_request(const sbp_chan_t *ch);
void sbpdev_end_request(void);

/** Reply channel: set for the duration of a deferred answer (scan results) and read to remember it. */
void sbpdev_set_channel(const sbp_chan_t *ch);
void sbpdev_channel(sbp_chan_t *ch);

/** Port (0 X1, 1 X2) a channel came through, -1 for the network or none. */
int sbpdev_chan_port(const sbp_chan_t *ch);

/** The port gets the module's unsolicited frames (a request came through it within SUB_TTL). */
bool sbpdev_port_subscribed(int port);

/** Somebody receives unsolicited frames: a subscribed port (X1 only while locked to SBP) or peer. */
bool sbpdev_has_listener(void);

/** Handle one queued frame (manager task). Returns 0 if handled, -1 if the id is not ours. */
int sbpdev_handle(const sbp_frame_t *f);

/** Send a frame from our address, carrying the mark bit: to the request being handled, else to every
 *  subscriber. */
void sbpdev_send(uint8_t type, uint8_t ver, uint8_t id, const uint8_t *payload, uint8_t len);

/** Send an unsolicited frame (a state change, a report) to every subscriber, also while a request from
 *  one channel is being handled. */
void sbpdev_notify(uint8_t type, uint8_t ver, uint8_t id, const uint8_t *payload, uint8_t len);

/** Acknowledge a SETTING/GETTING frame: CONTENT+resp, same version, payload {code, ck1, ck2}. */
void sbpdev_ack(const sbp_frame_t *req, uint8_t code);

/** Own SBP address of the module (ID_WIFI_NET v6, else 87 station / 88 access point) and its change. */
uint8_t sbpdev_route(void);
void sbpdev_set_route(uint8_t r);

/** Forget the mark bit, as a reset would (update window). */
void sbpdev_clear_mark(void);

/** Keep X1's current rate across the next reboot only (one-shot: updates, role reboot); X2's rate is saved
 *  by then. Also leaves the address for a 0.11 image, which 0.12 itself ignores. */
void sbpdev_save_resume(void);

/** Period of the unsolicited ID_WIFI v1 report, ms (0 = off). */
uint16_t sbpdev_report_ms(void);
void sbpdev_set_report_ms(uint16_t ms);
