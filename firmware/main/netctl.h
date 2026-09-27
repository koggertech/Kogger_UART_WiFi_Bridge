/*
 * netctl.h - ID_WIFI_NET (0x58) over Kogger SBP: role, access point, address and DHCP server,
 * UART lines and their UDP ports, line statistics, access point clients, own address while bridging.
 * Every SETTING carries the confirmation key. Contract: docs/SBP_WIFI.md; model: docs/NETWORK.md.
 */
#pragma once

#include "sbp.h"

/** Handle one ID_WIFI_NET frame (manager task; answers go to the frame's channel). */
void netctl_handle(const sbp_frame_t *f);
