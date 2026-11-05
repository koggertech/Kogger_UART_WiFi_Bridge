/*
 * profile.h - a module profile (0.22): every saved setting of a module in one blob, loaded whole with
 * ID_WIFI_PROFILE 0x5B (docs/SBP_WIFI.md 4c) and applied at once - all or nothing, then a reboot. The blob layout,
 * format 1, little-endian:
 *   U1 format, U1 name length (1..32), name (UTF-8), U1 role (0 station, 1 access point), U1 own address,
 *   U4 X1 rate, radio {U1 power 0.25 dBm, U1 protocols, U1 width, U1 power save},
 *   access point {U1 channel, U1 hidden, U1 max clients, U1 security, U1 len, SSID, U1 len, password},
 *   address and DHCP {ip[4], mask[4], U1 dhcp, pool start[4], pool end[4], U2 lease min, U1 offer},
 *   line 0 {U1 mode, U1 dest, ip[4], U2 rport, U2 lport},
 *   line 1 {U1 mode, U1 dest, ip[4], U2 rport, U2 lport, U4 rate, S1 TX pin, S1 RX pin, U1 input translator},
 *   U1 networks (0..PROFILE_NETS), each {U1 len, SSID, U1 len, password}; nothing after.
 * Here only the layout: lengths, the format and what fits in the fields. Whether the values make sense (pins,
 * channels, ports, rates) is checked by profctl.c with the module's own rules. Plain C, tested on the PC.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROFILE_FORMAT 1
#define PROFILE_MAX    1024  /**< the largest blob (8 networks with long names and passwords: 977 bytes) */
#define PROFILE_NETS   8     /**< saved networks a profile carries (as many as the module keeps) */

typedef struct {
    char ssid[33];
    char pass[65];
} profile_net_t;

typedef struct {
    uint8_t  mode, dest, ip[4];
    uint16_t rport, lport;
} profile_line_t;

typedef struct {
    char           name[33];
    uint8_t        role, address;
    uint32_t       x1_baud;
    uint8_t        radio[4];                       /**< power, protocols, width, power save (radio_cfg_t) */
    uint8_t        ap_channel, ap_hidden, ap_max_clients, ap_auth;
    char           ap_ssid[33], ap_pass[65];
    uint8_t        ip[4], mask[4], dhcp, pool_start[4], pool_end[4];
    uint16_t       lease_min;
    uint8_t        offer;
    profile_line_t line[2];
    uint32_t       x2_baud;
    int8_t         tx1, rx1;
    uint8_t        xlat1;
    uint8_t        nnets;
    profile_net_t  nets[PROFILE_NETS];
} profile_t;

/** Why a blob or a profile was refused: the CONTENT v3 answer (docs/SBP_WIFI.md 4c). 0 = fine. */
enum {
    PROFILE_OK = 0,
    PROFILE_E_INCOMPLETE = 1, /**< not all of it arrived, or no upload was begun */
    PROFILE_E_CRC,            /**< the checksum of what arrived differs from the announced one */
    PROFILE_E_FORMAT,         /**< another format, a length that does not fit, bytes left over */
    PROFILE_E_NAME,
    PROFILE_E_ROLE,
    PROFILE_E_ADDRESS,
    PROFILE_E_X1_BAUD,
    PROFILE_E_RADIO,
    PROFILE_E_AP,
    PROFILE_E_IP,
    PROFILE_E_LINE0,
    PROFILE_E_LINE1,
    PROFILE_E_LINES,          /**< both lines on the same UDP port */
    PROFILE_E_XLAT,           /**< an input translator this build does not carry */
    PROFILE_E_NETS,
    PROFILE_E_BUSY,           /**< an update runs, or a fresh image has not confirmed itself */
};

/** Blob -> profile. Returns PROFILE_OK or PROFILE_E_FORMAT / _NAME / _ROLE / _NETS for what the layout itself rules
 *  out (strings with a NUL inside, a role above 1, an address of 0 or 255, more networks than PROFILE_NETS). */
int profile_decode(const uint8_t *b, size_t n, profile_t *p);

/** Profile -> blob (at most PROFILE_MAX bytes); returns its length, 0 when it does not fit. */
size_t profile_encode(const profile_t *p, uint8_t *out, size_t cap);

/** CRC-32 (ISO-HDLC: zlib's crc32) of a blob: the upload check, and the fingerprint of the settings. */
uint32_t profile_crc32(const uint8_t *b, size_t n);
