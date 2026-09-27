/*
 * portinfo.h - what the module sees on each physical port (0 = X1, 1 = X2): traffic by protocol in both
 * directions, whether a host or devices sit there, and the devices heard (SBP address/board/firmware,
 * MAVLink system). Read by ID_WIFI_NET v7 (docs/SBP_WIFI.md); nothing here changes any traffic.
 *
 * Fed from several tasks: UART RX of each port (portinfo_up, portinfo_rx_bytes), the network task
 * (portinfo_down), the UART TX tasks (portinfo_tx_bytes) and the manager. Counters are single 32-bit
 * stores; the device table and the page snapshots run under the lock hooks given to portinfo_init().
 * Portable C99: tests/test_all.py builds it with a fake clock.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PI_PORTS     2
#define PI_MAXDEV    8     /**< devices kept per port; the stalest entry makes room */
#define PI_PAGE0_LEN 116
#define PI_PAGE1_MAX (3 + 12 * PI_MAXDEV)

/** Protocol of a unit, in the order of page 0. */
enum { PI_KP1, PI_KP2, PI_UBX, PI_MAV1, PI_MAV2, PI_RAW, PI_NPROTO };

/** Page 0 flags the caller knows (b2..b6); b0/b1 too. */
#define PI_F_UART      0x01u
#define PI_F_BRIDGE    0x02u
#define PI_F_ASKED     0x04u   /**< the request for this page came through this port */
#define PI_F_REPORTS   0x08u   /**< the port gets the module's unsolicited frames */
#define PI_F_SLIP      0x10u
#define PI_F_PROVISION 0x20u   /**< rate changed, waiting for confirmation */
#define PI_F_USB       0x40u

/** What is connected, judged over the last 10 s (page 0 byte 11). */
enum { PI_C_NONE, PI_C_UNREADABLE, PI_C_HOST, PI_C_DEVICE, PI_C_HOST_DEVICE, PI_C_MAVLINK, PI_C_UBLOX, PI_C_SLIP,
       PI_C_OTHER };

/** now_ds: a clock in 0.1 s; lock/unlock may be NULL (single-threaded tests). */
void portinfo_init(uint32_t (*now_ds)(void), void (*lock)(void), void (*unlock)(void));

/** Map a kframe unit (kind KF_FRAME/KF_RAW, flags KF_P_*) to PI_*. */
int portinfo_proto(int kf_kind, unsigned kf_flags);

void portinfo_rx_bytes(int port, size_t n);
void portinfo_tx_bytes(int port, size_t n);

/** One whole unit from the port (UART side); d/n is the frame from its sync, or the raw bytes. */
void portinfo_up(int port, int proto, const uint8_t *d, size_t n);

/** One whole unit written to the port (from the network). */
void portinfo_down(int port, int proto, const uint8_t *d, size_t n);

/** A request for the module arrived through the port / the module sent one of its frames there. */
void portinfo_module_rx(int port);
void portinfo_module_tx(int port);

/** Page 0 (PI_PAGE0_LEN bytes). flags: PI_F_*; slip: the port carries the IP bridge (judges byte 11). */
size_t portinfo_page0(int port, uint8_t flags, uint32_t baud, uint32_t saved, uint32_t rx_overflows,
                      uint32_t tx_drops, bool slip, uint8_t *out);

/** Page 1: devices heard from the port, most recent first. Returns its length (<= PI_PAGE1_MAX). */
size_t portinfo_page1(int port, uint8_t *out);

/** Age in 0.1 s of a clock value, 0xFFFF when never (0) or too old. */
uint16_t portinfo_age(uint32_t t);

#ifdef __cplusplus
}
#endif
