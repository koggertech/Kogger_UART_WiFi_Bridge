/*
 * ports.h - rates of the two physical ports, one rule for both (docs/SBP_WIFI.md, "Port rates"):
 * port 0 = X1 = line 0 (link.c), port 1 = X2 = line 1 (uline.c).
 *   - A change is applied and saved at once, whichever port or the network asked (0.15). A host that
 *     cannot follow the new rate gets the module back by holding BOOT 5 s: both ports return to the
 *     default (bootkey.h). Up to 0.14 a change of the asking port's own rate was provisional and went back
 *     after 10 s without a request at the new rate.
 * Saved rates: port 0 in NVS "baud" (the key 0.11 wrote with ID_FLASH, so a rollback keeps it), port 1
 * in "baud1" and its line record (netcfg.c). Manager task only, except ports_baud() (any task).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PORTS_N          2

/** After netcfg_load(): read the saved rate of port 0. */
void ports_load(void);

/** Current rate, 0 while the UART does not run. */
uint32_t ports_baud(int port);
uint32_t ports_saved_baud(int port);

/** Valid rate for a port. */
bool ports_baud_ok(uint32_t baud);

/**
 * Switch a port after everything queued so far (the acknowledgement) has left at the old rate, and save
 * the rate. A port whose UART does not run only has its saved rate changed (applies when it starts).
 * Returns false when the rate is out of range or not saved.
 */
bool ports_set_baud(int port, uint32_t baud);

/** ID_FLASH v0: save the current rates of both ports (every change is saved anyway since 0.15). */
bool ports_save_all(void);

/** ID_FLASH v2: forget the saved rates (defaults at the next boot); the current rates stay. */
bool ports_erase_saved(void);
