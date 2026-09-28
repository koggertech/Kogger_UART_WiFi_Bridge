/*
 * bootkey.h - resets with the BOOT button (docs/HARDWARE.md, "Resets with the BOOT button"), while the
 * firmware runs:
 *   - held BOOTKEY_RATES_MS: the LED blinks slowly; released now, the saved port rates return to the
 *     defaults (both 921600) and nothing else changes;
 *   - held BOOTKEY_ALL_MS: the LED blinks fast; released now, every setting returns to the factory one.
 * Either way the module restarts and resets before anything reads the settings. Shorter presses do
 * nothing, and so does a button that is already down when polling starts. At power-up the same button
 * selects the ROM download mode instead (the chip samples it before the firmware runs).
 * The poll step is plain C (tested on the PC, tests/test_all.py); main.c polls the pin and acts.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BOOTKEY_POLL_MS  50
#define BOOTKEY_RATES_MS 5000
#define BOOTKEY_ALL_MS   10000
#define BOOTKEY_SLOW_MS  250    /**< half period of the blink while the rate reset is armed */
#define BOOTKEY_FAST_MS  100    /**< half period of the blink while the full reset is armed */
#define BOOTKEY_UP_POLLS 2      /**< polls read "released" in a row that end a press: contact bounce is shorter */

typedef enum { BOOTKEY_NONE, BOOTKEY_RATES, BOOTKEY_ALL } bootkey_level_t;

typedef struct {
    uint32_t held_ms;   /**< the current press so far */
    uint8_t  up_polls;  /**< consecutive polls that read "released" */
    bool     seen_up;   /**< the button was released at least once since polling started */
    uint8_t  level;     /**< bootkey_level_t armed by the current press */
} bootkey_t;

typedef enum {
    BOOTKEY_LED_OFF,     /**< nothing armed: the LED is left alone */
    BOOTKEY_LED_ON,      /**< armed, blink phase on */
    BOOTKEY_LED_DARK,    /**< armed, blink phase off */
    BOOTKEY_RESET_RATES, /**< released with the rate reset armed */
    BOOTKEY_RESET_ALL    /**< released with the full reset armed */
} bootkey_act_t;

/** One poll, every BOOTKEY_POLL_MS: `down` = the button reads pressed. Start from a zeroed bootkey_t. */
bootkey_act_t bootkey_step(bootkey_t *k, bool down);
