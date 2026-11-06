/*
 * linexlat.h - line 1 as a listen-only tap on a GODIO controller -> receiver wire (F9 telemetry, 9600 8N1):
 * the line's input is not relayed as it is but translated to MAVLink (f9mav.h), which the relay then sends
 * over Wi-Fi like any MAVLink from a UART. Run with CONFIG_WB_LINE1_XLAT (Kconfig.projbuild; docs/GODIO_F9.md).
 *
 * In this mode uline.c runs UART1 at LINEXLAT_BAUD whatever is saved, leaves the TX pin alone (a tap must
 * never drive the wire) and drops everything queued for the UART. The module does not see its own SBP
 * frames on this line: nothing of the raw input reaches the relay's framer.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define LINEXLAT_BAUD 9600

/** Bytes from UART1. now_us: esp_timer_get_time(). */
void linexlat_bytes(const uint8_t *d, size_t n, int64_t now_us);

/** The line went quiet (end of the data for the framer) or the RX task just woke: heartbeat when due. */
void linexlat_idle(int64_t now_us);
