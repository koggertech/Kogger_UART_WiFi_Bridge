#include "bridge.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "lwip/lwip_napt.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"
#include "sdkconfig.h"

#include "frame.h"
#include "link.h"

#if !CONFIG_LWIP_IPV4_NAPT || !CONFIG_LWIP_IP_FORWARD
#error "Bridge needs CONFIG_LWIP_IPV4_NAPT and CONFIG_LWIP_IP_FORWARD (see sdkconfig.defaults)"
#endif

static const char *TAG = "bridge";

static struct netif s_nif;
static esp_netif_t *s_sta;
static bool s_ap_role;          /* an access point has no upstream network, hence no DNS to relay to */
static uint8_t s_scratch[FRAME_MAX_PAYLOAD]; /* used only from the lwIP thread */
static volatile bridge_stats_t s_st;

/* ---- lwIP netif ---------------------------------------------------------- */

static err_t br_output(struct netif *nif, struct pbuf *p, const ip4_addr_t *ipaddr)
{
    (void)nif;
    (void)ipaddr;
    if (p->tot_len > sizeof s_scratch)
        return ERR_OK; /* larger than MTU: cannot happen with mtu set, drop silently */
    u16_t n = pbuf_copy_partial(p, s_scratch, p->tot_len, 0);
    link_send(FRAME_T_IP, s_scratch, n);
    s_st.to_host++;
    return ERR_OK;
}

#if LWIP_IPV6
static err_t br_output_ip6(struct netif *nif, struct pbuf *p, const ip6_addr_t *ipaddr)
{
    (void)nif; (void)p; (void)ipaddr;
    return ERR_OK; /* IPv4-only bridge */
}
#endif

static err_t br_netif_init(struct netif *nif)
{
    nif->name[0] = 'h';
    nif->name[1] = 'u';
    nif->mtu = FRAME_MAX_PAYLOAD;
    nif->output = br_output;
#if LWIP_IPV6
    nif->output_ip6 = br_output_ip6;
#endif
    nif->flags = 0; /* point-to-point: no ARP, no broadcast */
    return ERR_OK;
}

void bridge_input(const uint8_t *pkt, size_t len)
{
    if (len < 20 || (pkt[0] >> 4) != 4) {
        s_st.from_host_bad++;
        return;
    }
    /* PBUF_LINK leaves headroom for the Ethernet header when the packet is forwarded to Wi-Fi. */
    struct pbuf *p = pbuf_alloc(PBUF_LINK, (u16_t)len, PBUF_RAM);
    if (!p) {
        s_st.from_host_bad++;
        return;
    }
    pbuf_take(p, pkt, (u16_t)len);
    if (s_nif.input(p, &s_nif) != ERR_OK) {
        pbuf_free(p);
        s_st.from_host_bad++;
        return;
    }
    s_st.from_host++;
}

static void add_netif_cb(void *arg)
{
    ip4_addr_t ip, mask, gw;
    ip4addr_aton(CONFIG_WB_BRIDGE_IP, &ip);
    ip4addr_aton(CONFIG_WB_HOST_IP, &gw);
    IP4_ADDR(&mask, 255, 255, 255, 252);
    netif_add(&s_nif, &ip, &mask, &gw, NULL, br_netif_init, tcpip_input);
    netif_set_up(&s_nif);
    netif_set_link_up(&s_nif);
    /* NAPT is enabled on the inside interface; lwIP translates what leaves via other netifs. */
    ip_napt_enable(ip.addr, 1);
    xSemaphoreGive((SemaphoreHandle_t)arg);
}

/* ---- DNS relay ----------------------------------------------------------- */

#define DNS_PENDING 32
#define DNS_TIMEOUT_MS 5000

typedef struct {
    uint16_t id;
    TickType_t t;
    struct sockaddr_in client;
    uint8_t used;
} dns_pending_t;

static bool upstream_dns(struct sockaddr_in *out)
{
    esp_netif_dns_info_t dns;
    if (s_ap_role || !s_sta || esp_netif_get_dns_info(s_sta, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK)
        return false; /* as AP the "DNS" of the netif is its own address, where nothing listens */
    if (dns.ip.type != ESP_IPADDR_TYPE_V4 || dns.ip.u_addr.ip4.addr == 0)
        return false;
    esp_netif_ip_info_t ipi;
    if (esp_netif_get_ip_info(s_sta, &ipi) != ESP_OK || ipi.ip.addr == 0)
        return false;
    memset(out, 0, sizeof *out);
    out->sin_family = AF_INET;
    out->sin_port = htons(53);
    out->sin_addr.s_addr = dns.ip.u_addr.ip4.addr;
    return true;
}

static void dns_task(void *arg)
{
    static dns_pending_t pend[DNS_PENDING];
    static uint8_t buf[768];

    int ls = socket(AF_INET, SOCK_DGRAM, 0);
    int us = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(53) };
    inet_aton(CONFIG_WB_BRIDGE_IP, &a.sin_addr);
    if (ls < 0 || us < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) != 0) {
        ESP_LOGE(TAG, "DNS relay: socket/bind failed");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "DNS relay on %s:53", CONFIG_WB_BRIDGE_IP);

    for (;;) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(ls, &rf);
        FD_SET(us, &rf);
        struct timeval tv = { .tv_sec = 1 };
        if (select((ls > us ? ls : us) + 1, &rf, NULL, NULL, &tv) <= 0)
            continue;

        if (FD_ISSET(ls, &rf)) {
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            int n = recvfrom(ls, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
            if (n >= 12) {
                s_st.dns_queries++;
                struct sockaddr_in up;
                if (!upstream_dns(&up)) {
                    buf[2] |= 0x80;                         /* QR = response */
                    buf[3] = (uint8_t)((buf[3] & 0xF0) | 2); /* RCODE = SERVFAIL */
                    sendto(ls, buf, n, 0, (struct sockaddr *)&from, fl);
                    s_st.dns_servfail++;
                } else {
                    TickType_t now = xTaskGetTickCount();
                    int slot = 0;
                    for (int i = 0; i < DNS_PENDING; i++) {
                        if (!pend[i].used || now - pend[i].t > pdMS_TO_TICKS(DNS_TIMEOUT_MS)) {
                            slot = i;
                            break;
                        }
                        if (pend[i].t < pend[slot].t)
                            slot = i; /* table full: reuse the oldest */
                    }
                    pend[slot] = (dns_pending_t){ .id = (uint16_t)(buf[0] << 8 | buf[1]),
                                                  .t = now, .client = from, .used = 1 };
                    sendto(us, buf, n, 0, (struct sockaddr *)&up, sizeof up);
                }
            }
        }

        if (FD_ISSET(us, &rf)) {
            int n = recvfrom(us, buf, sizeof buf, 0, NULL, NULL);
            if (n >= 12) {
                uint16_t id = (uint16_t)(buf[0] << 8 | buf[1]);
                for (int i = 0; i < DNS_PENDING; i++) {
                    if (pend[i].used && pend[i].id == id) {
                        sendto(ls, buf, n, 0, (struct sockaddr *)&pend[i].client, sizeof pend[i].client);
                        pend[i].used = 0;
                        break;
                    }
                }
            }
        }
    }
}

/* ---- public ------------------------------------------------------------- */

void bridge_start(esp_netif_t *sta, bool ap)
{
    s_sta = sta;
    s_ap_role = ap;
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(tcpip_callback(add_netif_cb, done) == ERR_OK ? ESP_OK : ESP_FAIL);
    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
    ESP_LOGI(TAG, "bridge %s <-> host %s, NAPT on", CONFIG_WB_BRIDGE_IP, CONFIG_WB_HOST_IP);
    xTaskCreate(dns_task, "dns_relay", 4096, NULL, 5, NULL);
}

void bridge_get_stats(bridge_stats_t *out)
{
    memcpy(out, (const void *)&s_st, sizeof *out);
}
