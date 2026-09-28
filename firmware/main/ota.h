/*
 * ota.h - firmware update over Kogger SBP, in the dialect KoggerApp already speaks to Kogger
 * bootloaders (ID_BOOT v0 -> ID_UPDATE chunks -> ID_BOOT v1), written into the inactive OTA slot,
 * validated before it is booted, and rolled back unless the new image proves it can talk to the host.
 * Procedure and safety argument: docs/UPDATE.md.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sbp.h"

#define SBP_ID_UPDATE 0x25

/* ID_UPDATE progress "type" (KoggerApp ID_UPGRADE_V0): 0..2 recoverable, above 2 fatal. */
#define OTA_T_ACCEPTED   0
#define OTA_T_REPOSITION 2   /**< host continues from lastNumMsg + 1 / lastOffset */
#define OTA_T_NO_SESSION 3   /**< chunk outside an update session */
#define OTA_T_BAD_IMAGE  4   /**< not an image of this firmware for this chip */
#define OTA_T_TOO_BIG    5   /**< larger than the OTA slot */
#define OTA_T_FLASH      6   /**< flash write failed */

/** Early at boot: read the running slot's OTA state (pending verification or not). */
void ota_init(void);

/** 0 = firmware, 1 = "bootloader" (update window open or transfer running): ID_VERSION v2 byte 0. */
uint8_t ota_boot_mode(void);

/** ID_BOOT v0 (key checked by the caller): open the 5 s update window; without a chunk the module reboots. */
void ota_boot_request(void);

/** ID_BOOT v1 (key checked): validate the received image. Returns the SBP response code; *reboot is
 *  set when a validated image is selected, and the caller then acknowledges and calls
 *  ota_reboot_into_new(). Without a session it is a no-op answered OK. */
uint8_t ota_run_request(bool *reboot);
void ota_reboot_into_new(void);

/** ID_UPDATE frame (manager task). Sends the progress frame itself. */
void ota_on_update(const sbp_frame_t *f);

/** A valid frame from the host was handled: evidence that a freshly updated image works. */
void ota_note_host_frame(void);

/** Called every 500 ms from the manager task: window/transfer timeouts and self-confirmation. */
void ota_tick(void);

/** This boot runs a freshly updated image that has not confirmed itself yet (read from any task). */
bool ota_pending(void);

/** Running slot label and state for diagnostics ("ota_0", "pending"/"valid"/"undefined"). */
const char *ota_running_label(void);
const char *ota_state_name(void);
