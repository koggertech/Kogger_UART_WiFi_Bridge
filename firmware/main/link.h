/*
 * link.h - byte transport to the host (USB Serial/JTAG or UART) with two framings on it:
 *   SLIP+CRC16 frames (IP bridge + text control, docs/PROTOCOL.md) - used by espwifi_bridge.py;
 *   Kogger SBP / KP1 frames (docs/SBP_WIFI.md) - used by KoggerApp directly.
 * Both decoders see every byte; the first valid frame after boot locks the link to its protocol
 * (the other one is ignored until reboot or an explicit link_set_proto()).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sbp.h"

typedef enum { LINK_PROTO_NONE = 0, LINK_PROTO_SLIP, LINK_PROTO_SBP } link_proto_t;

typedef struct {
    uint32_t rx_frames;      /**< valid SLIP frames */
    uint32_t rx_crc_errors;
    uint32_t rx_discarded;
    uint32_t sbp_rx_frames;  /**< valid SBP frames (any route) */
    uint32_t sbp_check_errors;
    uint32_t tx_frames;
    uint32_t tx_dropped;     /**< frames dropped because the TX queue was full or the host stalled */
    uint32_t rx_overflows;   /**< UART RX FIFO/ring-buffer overflows (bytes lost); always 0 on USB */
} link_stats_t;

/** Handlers run in the link RX task; they must not block for long, must copy what they keep, and
 *  must be ready to accept frames before link_start() is called. */
typedef void (*link_rx_handler_t)(const uint8_t *payload, size_t len);
typedef void (*link_sbp_handler_t)(const sbp_frame_t *f);

/** Install the transport driver and start the RX/TX tasks. */
void link_start(link_rx_handler_t on_ip, link_rx_handler_t on_ctl, link_sbp_handler_t on_sbp);

link_proto_t link_proto(void);

/** Lock the link to a protocol (NONE unlocks). SBP handlers call it when they accept a frame. */
void link_set_proto(link_proto_t p);

/**
 * Queue one SLIP frame. Safe from any task (including the lwIP thread); never blocks: if the
 * queue is full the frame is dropped and counted. Dropped while the link is locked to SBP.
 */
void link_send(uint8_t type, const uint8_t *payload, size_t len);

/** Queue one SBP frame (same rules). Dropped unless the link is locked to SBP. */
void link_send_sbp(uint8_t route, uint8_t mode, uint8_t id, const uint8_t *payload, uint8_t len);

/** Queue one complete unit (an SBP frame, a relayed frame or raw run) as is. All or nothing, so the
 *  host never receives a piece of a frame; false (and counted) when the TX ring is full. */
bool link_tx_frame(const uint8_t *d, size_t n);

/** Same for relayed units, which must leave the last kilobyte of the ring free: under overload the
 *  module's own answers and reports still get through. */
bool link_tx_relay(const uint8_t *d, size_t n);

/**
 * Change the UART baud rate after everything queued so far has been sent at the old rate.
 * @return false if the rate is outside LINK_BAUD_MIN..LINK_BAUD_MAX or the queue is full.
 *         On the USB Serial/JTAG transport the rate is only recorded (CDC ignores it).
 */
bool link_set_baud(uint32_t baud);

/** Current (or last requested, on USB) baud rate. */
uint32_t link_baud(void);

#define LINK_BAUD_MIN 9600u
#define LINK_BAUD_MAX 4000000u

void link_get_stats(link_stats_t *out);
