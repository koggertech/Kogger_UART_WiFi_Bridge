#include "ports.h"

#include "esp_log.h"
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

bool ports_set_baud(int port, uint32_t baud)
{
    if (!ok_port(port) || !ports_baud_ok(baud))
        return false;
    bool running = port == 0 || uline_running();
    if (!running)
        return save(port, baud); /* applies when the UART starts */
    uint32_t cur = ports_baud(port);
    if (baud != cur && !apply(port, baud))
        return false;
    bool ok = save(port, baud);
    ESP_LOGI(TAG, "port %d: %lu%s", port, (unsigned long)baud, ok ? ", saved" : ", NOT saved");
    return ok;
}

bool ports_save_all(void)
{
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
