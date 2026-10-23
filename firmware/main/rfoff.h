/*
 * rfoff.h - the radio switched off on request, to swap an antenna (ID_WIFI_RF 0x5A, docs/SBP_WIFI.md 4b): it stays
 * off until its timer runs out, an "on" request or a reboot; nothing of it is saved. A peer that reaches the module
 * over the network reaches it over that very radio, so it may switch it off only with a timer. The request check
 * and the answer are plain C (tested on the PC, tests/test_all.py); manager.c stops and starts the radio.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RFOFF_NET_MAX_S 600   /**< the longest switch-off a network peer may ask for */
#define RFOFF_STATE_LEN 3     /**< CONTENT v0 {U1 on, U2 seconds left} */

typedef struct {
    bool     on;    /**< switch on (true) or off */
    uint16_t secs;  /**< off: back on after this many seconds, 0 = only by an "on" request or a reboot */
} rfoff_req_t;

/** SETTING v0 {U4 key, U1 on, U2 seconds}: returns an SBP response code and fills *r on SBP_RESP_OK.
 *  `from_net`: the request came over the network, so switching off needs 1..RFOFF_NET_MAX_S seconds. */
uint8_t rfoff_parse(const uint8_t *p, size_t len, bool from_net, rfoff_req_t *r);

/** CONTENT v0: `left` = seconds until the timer switches the radio back on, 0 = no timer. */
void rfoff_state(uint8_t out[RFOFF_STATE_LEN], bool on, uint16_t left);
