/*
 * radio.h - Wi-Fi radio settings of the running interface (station or access point): transmit power
 * limit and connection mode (802.11 protocol set incl. Espressif Long Range, channel width, power
 * save). Saved in NVS and applied at boot; ID_WIFI v7 (docs/SBP_WIFI.md) reads and changes them.
 * Power save applies to the station only: an access point has to listen for its clients.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Protocol presets (bits = WIFI_PROTOCOL_11B/11G/11N/LR). */
#define RADIO_P_B      0x01
#define RADIO_P_BG     0x03
#define RADIO_P_BGN    0x07
#define RADIO_P_LR     0x08
#define RADIO_P_BGN_LR 0x0F

typedef struct {
    uint8_t power;  /**< max TX power, 0.25 dBm units, 8..80 (2..20 dBm; 81..84 accepted, stored as 80);
                         the radio rounds down to its steps */
    uint8_t proto;  /**< RADIO_P_* */
    uint8_t bw;     /**< 1 = 20 MHz, 2 = 40 MHz (needs 11n) */
    uint8_t ps;     /**< power save: 0 none, 1 min modem, 2 max modem */
} radio_cfg_t;

typedef struct {
    uint8_t power;  /**< TX power limit the driver actually uses, 0.25 dBm */
    uint8_t phy;    /**< negotiated with the AP: 0 LR, 1 11b, 2 11g, 4 HT20, 5 HT40; 0xFF not connected / AP role */
    uint8_t proto;  /**< protocol bitmap the driver holds for the interface (not negotiated) */
    uint8_t bw;     /**< channel width the driver holds (not negotiated: that is `phy` 4/5) */
} radio_now_t;

/** Load the saved settings; set protocol and width of the station or AP interface.
 *  After esp_wifi_set_mode(), before esp_wifi_start(). */
void radio_init(bool ap);

/** Transmit power and power save. After esp_wifi_start() and again after every connection. */
void radio_post_start(void);

bool radio_cfg_ok(const radio_cfg_t *c);

/**
 * Save and apply. Returns false if the driver refused (nothing saved). *reconnect is set when the
 * protocol or width changed: they take effect on the next association.
 */
bool radio_set(const radio_cfg_t *c, bool *reconnect);

void radio_get(radio_cfg_t *c, radio_now_t *now);
