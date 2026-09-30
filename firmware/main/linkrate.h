/*
 * Link rate: the PHY rates of the frames the module receives from its peer - the access point it is joined to, or
 * each station joined to it - counted in a short sniffer window when a host asks (ID_WIFI_SURVEY v1,
 * docs/SBP_WIFI.md 3b / 4a).
 *
 * A window, not all the time: ESP-IDF warns that the sniffer has a great impact on the throughput of a connection
 * (api-guides/wifi.rst, "Wi-Fi Sniffer Mode"), so it runs for LINKRATE_WINDOW_DEF ms per request and no sooner than
 * LINKRATE_GAP_MS after the last window.
 *
 * Decoded as documented for wifi_pkt_rx_ctrl_t: 802.11b/g by the PHY rate code of the frame, 802.11n by MCS,
 * bandwidth and guard interval. LR is not: ESP-IDF documents no encoding of LR frames in wifi_pkt_rx_ctrl_t (its
 * 5-bit rate field cannot hold the TX codes 0x29 and 0x2A), so a frame of an LR link is reported with its raw rate
 * field and a rate of 0 = not known, until a calibration against frames sent at a forced LR rate says what the field
 * means. A peer may send LR frames when the link negotiated LR (station) or the access point runs LR.
 *
 * Pure: no ESP-IDF calls, so the decoding and the pages are checked on the PC (tests/test_all.py). The sniffer
 * callback (Wi-Fi task) only calls linkrate_frame() while a window is open; the manager reads the pages after it
 * has closed the window, so no lock is needed.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define LINKRATE_PEERS_MAX  4    /**< an access point of this module takes up to 4 stations */
#define LINKRATE_RATES_MAX  8    /**< distinct rates remembered per peer; more are counted, not listed */
#define LINKRATE_TOP        4    /**< rates reported per peer, most frames first */
#define LINKRATE_WINDOW_MIN 50
#define LINKRATE_WINDOW_MAX 1000
#define LINKRATE_WINDOW_DEF 200  /**< ms of sniffing per request */
#define LINKRATE_GAP_MS     1000 /**< a new window no sooner than this after the last one ended */

/* Kind of a rate (byte 0 of an entry). */
#define LINKRATE_K_OTHER 0  /**< a frame of no kind this chip's band carries: counted, rate not known */
#define LINKRATE_K_B     1  /**< 802.11b DSSS/CCK; code = PHY rate code 0x00..0x07 */
#define LINKRATE_K_G     2  /**< 802.11g OFDM; code = PHY rate code 0x08..0x0F */
#define LINKRATE_K_HT    3  /**< 802.11n; code = MCS */
#define LINKRATE_K_LR    4  /**< a non-HT frame of an LR link; code = the raw 5-bit rate field, rate not known */

/* Flags of a rate (byte 2 of an entry). */
#define LINKRATE_F_SGI 0x01  /**< 802.11n short guard interval */
#define LINKRATE_F_40  0x02  /**< 40 MHz */

#define LINKRATE_PAGE_HEAD 16
#define LINKRATE_ENTRY     9
#define LINKRATE_PAGE_MAX  (LINKRATE_PAGE_HEAD + LINKRATE_TOP * LINKRATE_ENTRY)

/** kbit/s of a rate, 0 when not known (LR, or a code the 802.11 tables do not hold). */
uint32_t linkrate_kbps(uint8_t kind, uint8_t code, uint8_t flags);

/**
 * Kind, code and flags of a received frame from its wifi_pkt_rx_ctrl_t fields; `lr` = the frame comes from a peer
 * whose link may run LR, so a non-HT frame is reported as LR (its rate field is not an 802.11b/g code then).
 */
void linkrate_classify(uint8_t sig_mode, uint8_t rate, uint8_t mcs, bool cwb, bool sgi, bool lr,
                       uint8_t *kind, uint8_t *code, uint8_t *flags);

/** A window the module accepts: 0 = the default, others pulled into LINKRATE_WINDOW_MIN..MAX. */
uint16_t linkrate_clamp_window(uint16_t ms);

/**
 * Open a window: clears the counts. `peers` are the transmitter addresses whose frames count (up to
 * LINKRATE_PEERS_MAX), `lr[i]` whether peer i may send LR frames, `phy_mode` the PHY mode the link negotiated
 * (ESP-IDF wifi_phy_mode_t: 0 LR, 1 11b, 2 11g, 4 HT20, 5 HT40; 0xFF not known, as for an access point).
 */
void linkrate_begin(const uint8_t peers[][6], const bool *lr, int n, uint16_t window_ms, uint8_t phy_mode);

/** Sniffer callback: one data frame received without error; counted when `addr2` is one of the peers. */
void linkrate_frame(const uint8_t *addr2, int8_t rssi, uint8_t sig_mode, uint8_t rate, uint8_t mcs, bool cwb,
                    bool sgi);

/** How many peers there are to report (one page each). */
int linkrate_total(void);

/**
 * Page `index` into `out` (LINKRATE_PAGE_MAX bytes), the payload of CONTENT v1; returns its length, 0 when there
 * is no such page. U1 index, U1 total, U1[6] peer, U2 window ms, U2 frames from the peer, S1 their average RSSI
 * (-128 = none), U1 negotiated PHY mode, U1 n, U1 0, then n entries {U1 kind, U1 code, U1 flags, U4 kbit/s
 * (0 = not known), U2 frames}, the most frames first.
 */
int linkrate_page(int index, uint8_t *out);
