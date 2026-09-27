/*
 * uline.h - line 1: the second UART connector (UART1), bridged by relay.c. Same buffering rules as
 * line 0 (docs/RELAY.md): driver RX ring, TX ring of whole units, rate changes after queued data.
 * Started only when both pins are configured: an unknown board pin is never driven.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "netcfg.h"

/** Install UART1 with the line's pins and baud rate and start its tasks. */
bool uline_start(const line_cfg_t *c);

bool uline_running(void);

/** Queue one complete unit for the UART, all or nothing. */
bool uline_tx_frame(const uint8_t *d, size_t n);

/** Same for relayed units: the last kilobyte of the ring stays free for the module's own frames. */
bool uline_tx_relay(const uint8_t *d, size_t n);

bool uline_set_baud(uint32_t baud);

/** Current (or last requested) rate; when the last change took effect (esp_timer, 0 while queued). */
uint32_t uline_baud(void);
int64_t uline_baud_switched_us(void);

typedef struct {
    uint32_t rx_bytes, tx_bytes, rx_overflows, tx_drops;
} uline_stats_t;

void uline_get_stats(uline_stats_t *s);

/** Pins UART1 runs on (-1 while it does not run); saved pins apply at the next boot. */
void uline_pins(int8_t *tx, int8_t *rx);
