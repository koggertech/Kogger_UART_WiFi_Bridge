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

/** Where a frame for the module came from; its answers, and from then on the unsolicited frames, go there. */
typedef enum { CH_NONE = 0, CH_LINK = 1, CH_LINE1 = 2, CH_NET = 3 } sbp_chan_kind_t;

typedef struct {
    uint8_t  kind;   /**< sbp_chan_kind_t: host link (line 0), UART1 (line 1), network peer of a line */
    uint8_t  line;   /**< CH_NET: the line whose socket received the frame */
    uint16_t port;   /**< CH_NET: sender, network byte order */
    uint32_t ip;     /**< CH_NET: sender, network byte order */
} sbp_chan_t;

/** Load saved settings (address, report period, baud) and the one-shot resume record; sets the
 *  address. No link calls: before relay_start() and link_start(). */
void sbpdev_load(void);

/** Apply the loaded baud rate (resume record first, else the saved one). After link_start(). */
void sbpdev_start(void);

/** This boot's address came from the one-shot resume record (update or role reboot). */
bool sbpdev_route_resumed(void);

/** Link RX handler: accepts SETTING/GETTING frames for our address, locks the link to SBP, queues them. */
void sbpdev_on_frame(const sbp_frame_t *f);

/** Same for a frame from line 1 or from the network (relay.c). Only CH_LINK locks the link to SBP. */
void sbpdev_on_frame_ch(const sbp_frame_t *f, const sbp_chan_t *ch);

/** Manager task, before handling a frame: answers go to its channel, and so do unsolicited frames
 *  (state reports, scan results, update progress) until a frame arrives from another channel. */
void sbpdev_set_channel(const sbp_chan_t *ch);
void sbpdev_channel(sbp_chan_t *ch);

/** Somebody can receive unsolicited frames (the host link is locked to SBP, or a line/peer asked). */
bool sbpdev_has_listener(void);

/** Handle one queued frame (manager task). Returns 0 if handled, -1 if the id is not ours. */
int sbpdev_handle(const sbp_frame_t *f);

/** Send a frame from our address, carrying the mark bit. */
void sbpdev_send(uint8_t type, uint8_t ver, uint8_t id, const uint8_t *payload, uint8_t len);

/** Acknowledge a SETTING/GETTING frame: CONTENT+resp, same version, payload {code, ck1, ck2}. */
void sbpdev_ack(const sbp_frame_t *req, uint8_t code);

/** Current SBP address of the module, the one applied at boot, and a change of the current one. */
uint8_t sbpdev_route(void);
uint8_t sbpdev_default_route(void);
void sbpdev_set_route(uint8_t r);

/** Forget the mark bit, as a reset would (update window). */
void sbpdev_clear_mark(void);

/** Keep the current baud and address across the next reboot only (one-shot, used by updates). */
void sbpdev_save_resume(void);

/** Period of the unsolicited ID_WIFI v1 report, ms (0 = off). */
uint16_t sbpdev_report_ms(void);
void sbpdev_set_report_ms(uint16_t ms);
