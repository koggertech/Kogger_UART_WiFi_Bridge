/*
 * led.c - status LED pattern (led.h).
 */
#include "led.h"

bool led_step(led_t *s, bool linked, uint32_t now_ms)
{
    if (!linked) {
        s->linked = false;
        return true;
    }
    if (!s->linked) {
        s->linked = true;
        s->since_ms = now_ms;
    }
    return (uint32_t)(now_ms - s->since_ms) % (LED_FLASH_MS + LED_PAUSE_MS) < LED_FLASH_MS;
}
