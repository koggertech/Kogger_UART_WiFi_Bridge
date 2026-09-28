/*
 * ports.h - rates of the two physical ports, one rule for both (docs/SBP_WIFI.md, "Port rates"):
 * port 0 = X1 = line 0 (link.c), port 1 = X2 = line 1 (uline.c).
 *   - A change of the port the request came through is provisional: it is saved when a request for the
 *     module arrives through that port after the switch, or on ID_FLASH v0; otherwise the port returns
 *     to its previous rate after PORTS_CONFIRM_MS.
 *   - Any other change (the other port, or asked from the network) is saved at once.
 * Saved rates: port 0 in NVS "baud" (the key 0.11 wrote with ID_FLASH, so a rollback keeps it), port 1
 * in "baud1" and its line record (netcfg.c). Manager task only, except ports_baud() (any task).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PORTS_N          2
#define PORTS_CONFIRM_MS 10000

/** After netcfg_load(): read the saved rate of port 0. */
void ports_load(void);

/** Current rate, 0 while the UART does not run. */
uint32_t ports_baud(int port);
uint32_t ports_saved_baud(int port);
bool ports_provisional(int port);

/** Valid rate for a port. */
bool ports_baud_ok(uint32_t baud);

/**
 * Switch a port after everything queued so far (the acknowledgement) has left at the old rate.
 * provisional: the request came through this port. A port whose UART does not run only has its saved
 * rate changed (applies when it starts). Returns false when the rate is out of range or not saved.
 */
bool ports_set_baud(int port, uint32_t baud, bool provisional);

/** A request for the module arrived through the port; rx_us = when it was received (esp_timer). */
void ports_note_request(int port, int64_t rx_us);

/** Manager, every 500 ms: an unconfirmed change returns to the previous rate after its deadline. */
void ports_tick(void);

/** ID_FLASH v0: confirm and save the current rates of both ports. */
bool ports_save_all(void);

/** ID_FLASH v2: forget the saved rates (defaults at the next boot); the current rates stay. */
bool ports_erase_saved(void);
