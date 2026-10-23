/*
 * led.h - the status LED VD1 (docs/HARDWARE.md, "Status LED"): lit steadily while the module has no link, a short
 * flash every LED_FLASH_MS + LED_PAUSE_MS while it has one. A link is a station joined to its access point with an
 * address, or an access point with a station joined. While the BOOT button arms a reset its blink (bootkey.h)
 * takes the LED instead. The step is plain C (tested on the PC, tests/test_all.py); main.c polls it every
 * BOOTKEY_POLL_MS and drives the pin.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define LED_FLASH_MS 100   /**< with a link: lit this long, */
#define LED_PAUSE_MS 2000  /**< then dark this long */

typedef struct {
    bool     linked;    /**< the link was up at the last step */
    uint32_t since_ms;  /**< when it came up */
} led_t;

/** One poll: `linked` = the module has a link now, `now_ms` = a millisecond clock (may wrap). Returns true = lit.
 *  Start from a zeroed led_t. A link that comes up flashes at once. */
bool led_step(led_t *s, bool linked, uint32_t now_ms);
