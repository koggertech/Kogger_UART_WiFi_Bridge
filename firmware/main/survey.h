/*
 * survey.h - how busy each Wi-Fi channel is (ID_WIFI_SURVEY 0x59, docs/SBP_WIFI.md §4a), so that the
 * access point on a boat can be put on a quiet channel.
 *
 * The module sweeps the requested channels with a passive scan and, while it sweeps, counts every frame
 * the radio hears in promiscuous mode. "Busy" is the air time of those frames over the time spent on the
 * channel: it is Wi-Fi traffic only. Interference that is not Wi-Fi (Bluetooth, a microwave oven) is not
 * counted - the chip has no public channel-busy (CCA) counter - and neither are the gaps between frames
 * or the acknowledgements, so the figure is a floor, not the true occupancy.
 *
 * This file is the arithmetic and the accounting, with no ESP-IDF in it: it is tested on the PC
 * (tests/test_all.py). manager.c drives the radio and fills it in.
 *
 * Who writes what (no lock, and none needed): survey_begin() and survey_dwell_done() are the manager
 * task's, survey_frame() is the Wi-Fi task's promiscuous callback, and they touch different fields. The
 * manager reads the results only after promiscuous mode is off again.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SURVEY_CH_MIN      1
#define SURVEY_CH_MAX      13     /**< the contract's range: a mask bit exists for every 2.4 GHz channel */
#define SURVEY_CH_SCAN_MAX 11     /**< but the driver's country ("01") lets it visit only 1..11 */
#define SURVEY_CH_AP_MAX   11     /**< an access point of this module may use 1..11 (netcfg_ap_ok) */
#define SURVEY_DWELL_MIN   50
#define SURVEY_DWELL_MAX   1000
#define SURVEY_DWELL_DEF   120    /**< default time on one channel, ms */
#define SURVEY_SENDERS_MAX 24     /**< distinct transmitters remembered per channel */
#define SURVEY_PAGE_LEN    16     /**< bytes of one CONTENT v0 page (14 before 0.17) */

/* Flags, byte 13 of a page. */
#define SURVEY_F_HOME      0x01   /**< the module's own channel: its own traffic is in the figures */
#define SURVEY_F_AP_OK     0x02   /**< an access point of this module may use this channel (1..11) */
#define SURVEY_F_NOISE_BAD 0x04   /**< the noise figure never changed during the survey: do not trust it */

/**
 * Air time of one frame in microseconds, from the radio metadata of the promiscuous callback
 * (`wifi_pkt_rx_ctrl_t`): `sig_mode` 0 = 11b/g, 1 = 11n; `rate` is `wifi_phy_rate_t` for 11b/g; `mcs`
 * and `sgi` are for 11n at 20 MHz; `len` is `sig_len`, the frame including its checksum.
 * The preamble is included, the acknowledgement and the gaps around the frame are not. 0 = unknown rate.
 */
uint32_t survey_airtime_us(uint8_t sig_mode, uint8_t rate, uint8_t mcs, bool sgi, uint16_t len);

/** A dwell time the module accepts: out-of-range values are pulled to the ends, 0 = the default. */
uint16_t survey_clamp_dwell(uint16_t ms);

/** The channels the module will measure: bit0 = channel 1 … bit12 = channel 13, 0 = every channel. */
uint16_t survey_clamp_mask(uint16_t mask);

/**
 * Start: clears the results. `home` is the channel the module itself is on (0 if none), `own_bssid` the
 * BSSID of the module's own network (its own MAC as an access point, the access point's as a station,
 * NULL when it has none): the air time of that network is counted apart, so that a host choosing a
 * channel can take its own traffic out of the figure instead of guessing it.
 */
void survey_begin(uint16_t mask, uint16_t dwell_ms, uint8_t home, const uint8_t *own_bssid);

/** Manager: the radio has spent `ms` on `channel` (may be called again to add more time). */
void survey_dwell_done(uint8_t channel, uint16_t ms);

/**
 * Promiscuous callback: one frame heard on `channel`. `air_us` is survey_airtime_us(), `noise` the
 * radio's noise floor, `addr2` the transmitter's address or NULL when the frame has none (a control
 * frame, or a frame whose checksum failed - its air time still counts). `bssid` is the network the frame
 * belongs to, or NULL when it has none or cannot be trusted.
 */
void survey_frame(uint8_t channel, int8_t rssi, int8_t noise, uint32_t air_us, const uint8_t *addr2,
                  const uint8_t *bssid);

/** How many measured channels there are to report (those the radio really visited). */
int survey_total(void);

/**
 * Page `index` of `survey_total()` into `out` (SURVEY_PAGE_LEN bytes), the payload of CONTENT v0:
 * U1 index, U1 total, U1 channel, U2 dwell ms, U2 busy per mille, U2 frames, U2 transmitters,
 * S1 strongest RSSI, S1 noise floor dBm, U1 flags, U2 of the busy figure that is the module's own network
 * (0.17). False when there is no such page.
 */
bool survey_page(int index, uint8_t *out);
