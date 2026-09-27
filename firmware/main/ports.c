#include "ports.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "link.h"
#include "netcfg.h"
#include "uline.h"

static const char *TAG = "ports";

#if CONFIG_WB_LINK_UART
#define DEFAULT0 CONFIG_WB_LINK_UART_BAUD
#else
#define DEFAULT0 921600u
#endif

static uint32_t s_saved0 = DEFAULT0;

/* One provisional change per port. The confirmation window runs from the moment the UART really
 * switched (the acknowledgement may still be queued behind relayed data at a low rate). */
static struct {
    bool     on;
    uint32_t old;            /* rate to return to */
    int64_t  asked;          /* when the change was requested */
} s_prov[PORTS_N];

void ports_load(void)
{
    nvs_handle_t h;
    uint32_t b;
    if (nvs_open("wb", NVS_READONLY, &h) != ESP_OK)
        return;
    if (nvs_get_u32(h, "baud", &b) == ESP_OK && ports_baud_ok(b))
        s_saved0 = b;
    nvs_close(h);
}

bool ports_baud_ok(uint32_t baud)
{
    return baud >= LINK_BAUD_MIN && baud <= LINK_BAUD_MAX;
}

static bool ok_port(int port)
{
    return port == 0 || port == 1;
}

uint32_t ports_baud(int port)
{
    if (port == 0)
        return link_baud();
    return port == 1 && uline_running() ? uline_baud() : 0;
}

uint32_t ports_saved_baud(int port)
{
    if (port == 0)
        return s_saved0;
    line_cfg_t c;
    netcfg_line(1, &c);
    return c.baud;
}

bool ports_provisional(int port)
{
    return ok_port(port) && s_prov[port].on;
}

static bool save0(uint32_t baud)
{
    nvs_handle_t h;
    if (nvs_open("wb", NVS_READWRITE, &h) != ESP_OK)
        return false;
    bool ok = nvs_set_u32(h, "baud", baud) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok)
        s_saved0 = baud;
    return ok;
}

static bool save(int port, uint32_t baud)
{
    return port == 0 ? save0(baud) : netcfg_set_baud1(baud);
}

static bool apply(int port, uint32_t baud)
{
    if (port == 0)
        return link_set_baud(baud);
    return uline_set_baud(baud);
}

static int64_t switched_us(int port)
{
    return port == 0 ? link_baud_switched_us() : uline_baud_switched_us();
}

bool ports_set_baud(int port, uint32_t baud, bool provisional)
{
    if (!ok_port(port) || !ports_baud_ok(baud))
        return false;
    bool running = port == 0 || uline_running();
    if (!running)
        return save(port, baud); /* applies when the UART starts */
    uint32_t cur = ports_baud(port);
    if (provisional) {
        if (!s_prov[port].on) {
            s_prov[port].old = cur;
            s_prov[port].on = true;
        }
        s_prov[port].asked = esp_timer_get_time();
        if (baud == s_prov[port].old) /* back to where it was: nothing left to confirm */
            s_prov[port].on = false;
        ESP_LOGI(TAG, "port %d: %lu provisional", port, (unsigned long)baud);
        return baud == cur || apply(port, baud);
    }
    s_prov[port].on = false;
    if (baud != cur && !apply(port, baud))
        return false;
    return save(port, baud);
}

void ports_note_request(int port, int64_t rx_us)
{
    if (!ports_provisional(port))
        return;
    int64_t sw = switched_us(port);
    if (sw == 0 || rx_us <= sw || rx_us <= s_prov[port].asked)
        return; /* not switched yet, or the frame came in at the old rate */
    s_prov[port].on = false;
    uint32_t b = ports_baud(port);
    bool ok = save(port, b);
    ESP_LOGI(TAG, "port %d: %lu confirmed%s", port, (unsigned long)b, ok ? "" : ", NOT saved");
}

void ports_tick(void)
{
    int64_t now = esp_timer_get_time();
    for (int p = 0; p < PORTS_N; p++) {
        if (!s_prov[p].on)
            continue;
        int64_t sw = switched_us(p);
        if (sw == 0)
            continue; /* the acknowledgement is still on its way at the old rate */
        int64_t from = sw > s_prov[p].asked ? sw : s_prov[p].asked;
        if (now - from < (int64_t)PORTS_CONFIRM_MS * 1000)
            continue;
        s_prov[p].on = false;
        ESP_LOGW(TAG, "port %d: no request at %lu, back to %lu", p, (unsigned long)ports_baud(p),
                 (unsigned long)s_prov[p].old);
        apply(p, s_prov[p].old);
    }
}

bool ports_save_all(void)
{
    s_prov[0].on = s_prov[1].on = false;
    bool ok = save0(link_baud());
    if (uline_running())
        ok = netcfg_set_baud1(uline_baud()) && ok;
    return ok;
}

bool ports_erase_saved(void)
{
    nvs_handle_t h;
    if (nvs_open("wb", NVS_READWRITE, &h) != ESP_OK)
        return false;
    nvs_erase_key(h, "baud");
    bool ok = nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok)
        s_saved0 = DEFAULT0;
    return netcfg_erase_baud1() && ok;
}
