/*
 * profctl.h - ID_WIFI_PROFILE 0x5B (0.22, docs/SBP_WIFI.md 4c): a module profile (profile.h) uploaded in pieces,
 * checked whole with the module's own rules, saved at once and run after a reboot - all or nothing; and which
 * profile the module carries, and whether its settings changed since. Manager task only.
 */
#pragma once

#include "sbp.h"

/** One ID_WIFI_PROFILE request (SETTING/GETTING); answers on the request's channel. */
void profctl_handle(const sbp_frame_t *f);
