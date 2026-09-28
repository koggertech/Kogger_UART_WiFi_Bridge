/*
 * HeadUnit Wi-Fi module (ESP32-C3): a UART <-> Wi-Fi bridge in the station or access point role.
 *  - to a Kogger SBP host it is a device, board 87, controlled with ID_WIFI 0x57 and ID_WIFI_NET 0x58
 *    (docs/SBP_WIFI.md);
 *  - its two ports (X1 = line 0, X2 = line 1) are equal: the module answers on either, and each can be
 *    bridged to its own UDP/TCP port (docs/RELAY.md, docs/NETWORK.md);
 *  - to espwifi_bridge.py it is an IP bridge with NAPT into its Wi-Fi interface on X1 (docs/PROTOCOL.md).
 * On X1 the first valid SBP or SLIP frame after boot picks the protocol. Architecture: docs/DESIGN.md.
 */
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "bootkey.h"
#include "bridge.h"
#include "link.h"
#include "manager.h"
#include "netcfg.h"
#include "ota.h"
#include "portinfo.h"
#include "ports.h"
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

/* Resets with the BOOT button (bootkey.h). The request crosses esp_restart() in RTC memory and the reset
 * runs at the next boot before anything reads a setting, so no task can write one back in between. Two
 * words, so that power-up garbage cannot pass for a request, and only a software reset counts. The
 * button does nothing while a freshly updated image is unconfirmed: the restart would roll it back, and
 * the old image would leave the request lying for the next update to find. */
#define BOOTKEY_GPIO 9           /* SB1 to GND, 10 kOhm pull-up (docs/HARDWARE.md) */
#define LED_GPIO     0           /* VD1, lit when high; X2's pins may be moved onto it (netcfg_pin_ok) */
#define RESET_MAGIC  0x57b0c1a5u /* + BOOTKEY_RESET_RATES or BOOTKEY_RESET_ALL */

static RTC_NOINIT_ATTR uint32_t s_reset_req[2];

/** The reset the previous run asked for: BOOTKEY_RESET_RATES, BOOTKEY_RESET_ALL or 0. Consumed. */
static uint32_t reset_request(void)
{
    uint32_t r = s_reset_req[0] - RESET_MAGIC;
    bool ok = s_reset_req[1] == ~s_reset_req[0] && (r == BOOTKEY_RESET_RATES || r == BOOTKEY_RESET_ALL)
              && esp_reset_reason() == ESP_RST_SW;
    s_reset_req[0] = s_reset_req[1] = 0; /* before acting: a failing reset must not loop */
    return ok ? r : 0;
}

static void bootkey_poll(void *arg)
{
    static bootkey_t k;
    static int led = -1;  /* -1 not decided yet, 0 X2 uses the pin, 1 ours */
    static uint8_t told;  /* level already logged */
    (void)arg;
    if (ota_pending()) {
        k = (bootkey_t){0};
        return;
    }
    bootkey_act_t a = bootkey_step(&k, gpio_get_level(BOOTKEY_GPIO) == 0);
    if (a == BOOTKEY_RESET_RATES || a == BOOTKEY_RESET_ALL) {
        s_reset_req[0] = RESET_MAGIC + (uint32_t)a;
        s_reset_req[1] = ~s_reset_req[0];
        ESP_LOGW("main", "BOOT released: restarting to reset %s",
                 a == BOOTKEY_RESET_ALL ? "every setting" : "the port rates");
        esp_restart();
    }
    if (k.level != told) {
        told = k.level;
        if (told != BOOTKEY_NONE)
            ESP_LOGW("main", "BOOT held %d s: %s on release",
                     (told == BOOTKEY_ALL ? BOOTKEY_ALL_MS : BOOTKEY_RATES_MS) / 1000,
                     told == BOOTKEY_ALL ? "every setting is reset" : "the port rates are reset");
    }
    if (a == BOOTKEY_LED_OFF)
        return;
    if (led < 0) {
        int8_t tx, rx;
        uline_pins(&tx, &rx);
        led = tx != LED_GPIO && rx != LED_GPIO;
        const gpio_config_t io = {.pin_bit_mask = 1ull << LED_GPIO, .mode = GPIO_MODE_OUTPUT};
        if (led && gpio_config(&io) != ESP_OK)
            led = 0;
    }
    if (led)
        gpio_set_level(LED_GPIO, a == BOOTKEY_LED_ON);
}

static void bootkey_start(void)
{
    const gpio_config_t io = {.pin_bit_mask = 1ull << BOOTKEY_GPIO, .mode = GPIO_MODE_INPUT,
                              .pull_up_en = GPIO_PULLUP_ENABLE};
    const esp_timer_create_args_t ta = {.callback = bootkey_poll, .name = "bootkey"};
    esp_timer_handle_t t;
    if (gpio_config(&io) != ESP_OK || esp_timer_create(&ta, &t) != ESP_OK
        || esp_timer_start_periodic(t, BOOTKEY_POLL_MS * 1000) != ESP_OK)
        ESP_LOGE("main", "BOOT button not polled: no factory reset");
}

void app_main(void)
{
    uint32_t reset = reset_request();
    if (reset == BOOTKEY_RESET_ALL) {
        ESP_LOGW("main", "BOOT reset: erasing every saved setting");
        ESP_ERROR_CHECK(nvs_flash_erase());
    }
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    if (reset == BOOTKEY_RESET_RATES) { /* before anything reads the rates; nothing else runs yet */
        bool ok = ports_erase_saved();
        ESP_LOGW("main", "BOOT reset: saved port rates %s", ok ? "erased, both ports at the default" : "NOT erased");
    }
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
    bootkey_start();
}
