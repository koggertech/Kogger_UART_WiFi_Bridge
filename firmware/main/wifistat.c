#include "wifistat.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"

static const char *TAG = "wifistat";

static esp_netif_t *s_nif;
static netif_input_fn s_in_orig;
static netif_linkoutput_fn s_out_orig;
static uint32_t s_rx, s_tx;          /* updated from the Wi-Fi task and the lwIP thread */
static uint32_t s_prev_rx, s_prev_tx;
static volatile uint32_t s_rx_bps, s_tx_bps;

static err_t in_wrap(struct pbuf *p, struct netif *n)
{
    __atomic_fetch_add(&s_rx, p->tot_len, __ATOMIC_RELAXED);
    return s_in_orig(p, n);
}

static err_t out_wrap(struct netif *n, struct pbuf *p)
{
    __atomic_fetch_add(&s_tx, p->tot_len, __ATOMIC_RELAXED);
    return s_out_orig(n, p);
}

/* Runs in the lwIP thread. esp_netif re-adds the netif after a Wi-Fi restart, which resets
 * input/linkoutput, so this is repeated on every GOT_IP / AP start and is idempotent. */
static void hook_cb(void *arg)
{
    struct netif *n = esp_netif_get_netif_impl(s_nif);
    if (!n || !n->input || !n->linkoutput)
        return;
    if (n->input != in_wrap) {
        s_in_orig = n->input;
        __atomic_signal_fence(__ATOMIC_SEQ_CST); /* original must be visible before the hook */
        n->input = in_wrap;
    }
    if (n->linkoutput != out_wrap) {
        s_out_orig = n->linkoutput;
        __atomic_signal_fence(__ATOMIC_SEQ_CST);
        n->linkoutput = out_wrap;
    }
}

static void on_up(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (tcpip_callback(hook_cb, NULL) != ERR_OK)
        ESP_LOGW(TAG, "could not install traffic hooks");
}

static void tick(void *arg)
{
    uint32_t rx = __atomic_load_n(&s_rx, __ATOMIC_RELAXED);
    uint32_t tx = __atomic_load_n(&s_tx, __ATOMIC_RELAXED);
    s_rx_bps = rx - s_prev_rx;
    s_tx_bps = tx - s_prev_tx;
    s_prev_rx = rx;
    s_prev_tx = tx;
}

void wifistat_start(esp_netif_t *netif, bool ap)
{
    s_nif = netif;
    if (ap) /* after esp_netif's own AP_START handler (registered earlier): the netif is up by then */
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START, on_up, NULL));
    else
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_up, NULL));
    const esp_timer_create_args_t ta = { .callback = tick, .name = "wifistat" };
    esp_timer_handle_t t;
    ESP_ERROR_CHECK(esp_timer_create(&ta, &t));
    ESP_ERROR_CHECK(esp_timer_start_periodic(t, 1000 * 1000));
}

void wifistat_get(wifistat_t *out)
{
    out->rx_total = __atomic_load_n(&s_rx, __ATOMIC_RELAXED);
    out->tx_total = __atomic_load_n(&s_tx, __ATOMIC_RELAXED);
    out->rx_bps = s_rx_bps;
    out->tx_bps = s_tx_bps;
}
