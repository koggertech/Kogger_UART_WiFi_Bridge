/*
 * HeadUnit Wi-Fi module (ESP32-C3): a UART <-> Wi-Fi bridge in the station or access point role.
 *  - to a Kogger SBP host it is a device, board 87, controlled with ID_WIFI 0x57 and ID_WIFI_NET 0x58
 *    (docs/SBP_WIFI.md);
 *  - its two ports (X1 = line 0, X2 = line 1) are equal: the module answers on either, and each can be
 *    bridged to its own UDP/TCP port (docs/RELAY.md, docs/NETWORK.md);
 *  - to espwifi_bridge.py it is an IP bridge with NAPT into its Wi-Fi interface on X1 (docs/PROTOCOL.md).
 * On X1 the first valid SBP or SLIP frame after boot picks the protocol. Architecture: docs/DESIGN.md.
 */
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "bridge.h"
#include "link.h"
#include "manager.h"
#include "netcfg.h"
#include "ota.h"
#include "portinfo.h"
#include "radio.h"
#include "relay.h"
#include "sbpdev.h"
#include "uline.h"
#include "wifiap.h"
#include "wifistat.h"

static portMUX_TYPE s_pi_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t pi_now(void)
{
    return (uint32_t)(esp_timer_get_time() / 100000); /* 0.1 s */
}

static void pi_lock(void)
{
    taskENTER_CRITICAL(&s_pi_mux);
}

static void pi_unlock(void)
{
    taskEXIT_CRITICAL(&s_pi_mux);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ota_init();             /* is this boot a fresh update that still has to confirm itself? */
    netcfg_load();          /* role, access point, address/DHCP, lines: everything below depends on them */
    bool ap = netcfg_role() == ROLE_AP;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *net = ap ? esp_netif_create_default_wifi_ap() : esp_netif_create_default_wifi_sta();
    if (!ap)
        esp_netif_set_hostname(net, CONFIG_WB_HOSTNAME);

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM)); /* our settings live in our own NVS records */
    ESP_ERROR_CHECK(esp_wifi_set_mode(ap ? WIFI_MODE_AP : WIFI_MODE_STA));
    /* SSID, security, address, DHCP server: before the radio starts. If the driver refuses even the
     * built-in defaults the radio is not started at all: its own configuration is an open network. */
    bool radio_ok = !ap || wifiap_setup(net);
    radio_init(ap);         /* saved protocol set and channel width: before the radio starts */

    /* Everything that takes link frames, line bytes or Wi-Fi events is ready before any can arrive. */
    bridge_start(net, ap);
    manager_init(net);      /* queue + Wi-Fi/IP event handlers (WIFI_EVENT_AP_START follows the start) */
    portinfo_init(pi_now, pi_lock, pi_unlock); /* port statistics: before any UART byte is counted */
    relay_init();           /* line buffers + saved configuration: the RX tasks may feed it at once */
    line_cfg_t l1;
    netcfg_line(1, &l1);
    uline_start(&l1);       /* second connector, if its pins are configured */
    sbpdev_load();          /* own address, report period, saved rates, one-shot resume record */
    relay_set_netif(net);
    relay_start();          /* own address and SBP lock: before any frame is judged */
    link_start(bridge_input, manager_on_ctl);
    sbpdev_start();         /* may queue the saved baud rate: before anything is sent */
    wifistat_start(net, ap);

    if (radio_ok) {
        ESP_ERROR_CHECK(esp_wifi_start());
        radio_post_start(); /* saved transmit power and power save: only after start */
    } else {
        ESP_LOGE("main", "access point settings refused: radio off, the module stays reachable over UART");
    }
    manager_start();
}
