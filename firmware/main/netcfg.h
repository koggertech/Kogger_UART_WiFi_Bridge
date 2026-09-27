/*
 * netcfg.h - network configuration of the module, kept in NVS (docs/NETWORK.md):
 *   role        station (joins a network) or access point (runs its own network);
 *   AP          SSID, password, security, channel, hidden, client limit;
 *   IP / DHCP   the access point's address and its DHCP server;
 *   lines       each physical UART line bridged to its own UDP port (line 0 = UART0, the host link;
 *               line 1 = UART1, the second connector), and the module's own SBP address while bridging.
 * Validation lives here; applying is done by wifiap.c, relay.c and uline.c.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NLINES 2

typedef enum { ROLE_STA = 0, ROLE_AP = 1 } wifi_role_t;

typedef struct {
    char    ssid[33];
    char    pass[65];
    uint8_t channel;      /**< 1..11 (driver's default country) */
    uint8_t hidden;       /**< 0/1 */
    uint8_t max_clients;  /**< 1..10 */
    uint8_t auth;         /**< 0 open, 1 WPA2-PSK, 2 WPA2/WPA3-PSK */
} ap_cfg_t;

#define IPCFG_OFFER_GW  1u    /**< DHCP offers the AP as gateway */
#define IPCFG_OFFER_DNS 2u    /**< DHCP offers the AP as DNS server */

typedef struct {
    uint8_t  ip[4];
    uint8_t  mask[4];
    uint8_t  dhcp;        /**< DHCP server on/off */
    uint8_t  pool_start[4];
    uint8_t  pool_end[4];
    uint16_t lease_min;   /**< 1..2880 minutes */
    uint8_t  offer;       /**< IPCFG_OFFER_GW | IPCFG_OFFER_DNS */
} ip_cfg_t;

typedef enum { LINE_OFF = 0, LINE_UDP = 1, LINE_TCP = 2 } line_mode_t;
typedef enum { DEST_FIXED = 0, DEST_SENDERS = 1, DEST_BROADCAST = 2 } line_dest_t;

typedef struct {
    uint8_t  mode;        /**< line_mode_t; TCP = client (meant for the station role) */
    uint8_t  dest;        /**< line_dest_t: UDP goes to ip:rport / to whoever sent to lport / to the subnet */
    uint8_t  ip[4];
    uint16_t rport;
    uint16_t lport;       /**< own UDP port; 0 = rport */
    uint32_t baud;        /**< line 1 only (line 0 follows ID_UART) */
    int8_t   tx_pin;      /**< line 1 only (default 5 = X2 pin 1), -1 = not wired: UART1 stays off */
    int8_t   rx_pin;      /**< line 1 only (default 4 = X2 pin 2) */
} line_cfg_t;

/** Load everything (and migrate the 0.7 "relay" record into line 0). Call once, early. */
void netcfg_load(void);

wifi_role_t netcfg_role(void);                /**< role the firmware runs in (fixed until reboot) */
wifi_role_t netcfg_saved_role(void);          /**< role of the next boot */
bool netcfg_set_role(wifi_role_t r);          /**< saved; takes effect after a reboot */

/** Forget saved lines and own address (also the 0.7 record): the next boot uses its role's defaults. */
bool netcfg_forget_lines(void);

void netcfg_ap(ap_cfg_t *c);
void netcfg_ap_default(ap_cfg_t *c);          /**< Kogger-XXXX, WPA2, documented password, channel 6 */
bool netcfg_ap_ok(const ap_cfg_t *c);
bool netcfg_set_ap(const ap_cfg_t *c);

void netcfg_ip(ip_cfg_t *c);
bool netcfg_ip_ok(const ip_cfg_t *c);
bool netcfg_set_ip(const ip_cfg_t *c);

void netcfg_line(int line, line_cfg_t *c);
bool netcfg_line_ok(int line, const line_cfg_t *c);
bool netcfg_set_line(int line, const line_cfg_t *c);

/** Pins usable for the second UART on ESP32-C3-MINI-1U (no strapping, flash, USB or UART0 pins). */
bool netcfg_pin_ok(int pin);

uint8_t netcfg_addr(void);                     /**< module SBP address while any line bridges */
bool netcfg_set_addr(uint8_t a);               /**< 1..255; 0 is refused */
