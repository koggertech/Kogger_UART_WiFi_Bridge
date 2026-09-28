/*
 * bootkey.c - poll step of the BOOT button resets (bootkey.h).
 */
#include "bootkey.h"

static bootkey_act_t led(const bootkey_t *k)
{
    uint32_t phase;
    if (k->level == BOOTKEY_ALL)
        phase = (k->held_ms - BOOTKEY_ALL_MS) / BOOTKEY_FAST_MS;
    else if (k->level == BOOTKEY_RATES)
        phase = (k->held_ms - BOOTKEY_RATES_MS) / BOOTKEY_SLOW_MS;
    else
        return BOOTKEY_LED_OFF;
    return phase % 2 ? BOOTKEY_LED_DARK : BOOTKEY_LED_ON;
}

bootkey_act_t bootkey_step(bootkey_t *k, bool down)
{
    if (!down) {
        k->seen_up = true;
        if (k->up_polls < BOOTKEY_UP_POLLS)
            k->up_polls++;
        if (k->up_polls < BOOTKEY_UP_POLLS)
            return led(k); /* one poll can be bounce: the press goes on */
        if (k->level == BOOTKEY_ALL)
            return BOOTKEY_RESET_ALL;
        if (k->level == BOOTKEY_RATES)
            return BOOTKEY_RESET_RATES;
        k->held_ms = 0;
        return BOOTKEY_LED_OFF;
    }
    if (!k->seen_up)
        return BOOTKEY_LED_OFF; /* down since polling started: not a press anyone made on purpose */
    k->up_polls = 0;
    if (k->held_ms < 0x7fffffffu)
        k->held_ms += BOOTKEY_POLL_MS;
    if (k->held_ms >= BOOTKEY_ALL_MS)
        k->level = BOOTKEY_ALL;
    else if (k->held_ms >= BOOTKEY_RATES_MS)
        k->level = BOOTKEY_RATES;
    return led(k);
}
