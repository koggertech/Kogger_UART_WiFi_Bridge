#include "uline.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "link.h"
#include "portinfo.h"
#include "relay.h"

static const char *TAG = "uline";

#define UNUM          UART_NUM_1
#define RX_RING       8192
#define DRV_TX_RING   0       /* no driver ring: uart_write_bytes sleeps on the FIFO (see link.c) */
#define TX_RING       8192
#define TX_RESERVE    1024    /* kept for the module's own frames */
#define LONG_IDLE_MS  20
#define LINE_TICKS(ms) (pdMS_TO_TICKS(ms) > 0 ? pdMS_TO_TICKS(ms) : 1) /* never 0 ticks at HZ=100 */

static bool s_run;
static int8_t s_tx_pin = -1, s_rx_pin = -1;
static QueueHandle_t s_evq;
static uint8_t *s_ring;
static size_t s_head, s_tail, s_mark;
static volatile uint32_t s_baud, s_pending_baud;
static volatile int64_t s_switched_us;  /* when the last rate change took effect */
static SemaphoreHandle_t s_mux;
static TaskHandle_t s_tx;
static volatile uline_stats_t s_st;

bool uline_running(void)
{
    return s_run;
}

static bool tx_put(const uint8_t *d, size_t n, size_t reserve)
{
    if (!s_run || n == 0)
        return s_run;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    size_t used = (s_head + TX_RING - s_tail) % TX_RING;
    bool fits = n + reserve <= TX_RING - 1 - used;
    if (fits) {
        size_t first = TX_RING - s_head < n ? TX_RING - s_head : n;
        memcpy(s_ring + s_head, d, first);
        memcpy(s_ring, d + first, n - first);
        s_head = (s_head + n) % TX_RING;
    }
    xSemaphoreGive(s_mux);
    if (!fits) {
        s_st.tx_drops++; /* whole unit dropped: the device never sees a piece of a frame */
        return false;
    }
    xTaskNotifyGive(s_tx);
    return true;
}

bool uline_tx_frame(const uint8_t *d, size_t n)
{
    return tx_put(d, n, 0);
}

bool uline_tx_relay(const uint8_t *d, size_t n)
{
    return tx_put(d, n, TX_RESERVE);
}

bool uline_set_baud(uint32_t baud)
{
    if (!s_run || baud < LINK_BAUD_MIN || baud > LINK_BAUD_MAX)
        return false;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_mark = s_head;
    s_pending_baud = baud;
    xSemaphoreGive(s_mux);
    xTaskNotifyGive(s_tx);
    return true;
}

static void tx_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, LINE_TICKS(100));
        for (;;) {
            xSemaphoreTake(s_mux, portMAX_DELAY);
            bool pend = s_pending_baud != 0;
            size_t end = pend ? s_mark : s_head;
            size_t n = end >= s_tail ? end - s_tail : TX_RING - s_tail;
            const uint8_t *p = s_ring + s_tail;
            xSemaphoreGive(s_mux);
            if (n == 0) {
                if (pend) { /* everything queued before the request has left: switch */
                    xSemaphoreTake(s_mux, portMAX_DELAY);
                    uint32_t b = s_pending_baud;
                    xSemaphoreGive(s_mux);
                    uart_wait_tx_done(UNUM, LINE_TICKS(500));
                    uart_set_baudrate(UNUM, b);
                    s_baud = b;
                    s_switched_us = esp_timer_get_time();
                    xSemaphoreTake(s_mux, portMAX_DELAY);
                    if (s_pending_baud == b)
                        s_pending_baud = 0; /* a newer request keeps its own mark and runs next */
                    xSemaphoreGive(s_mux);
                    continue;
                }
                break;
            }
            int w = uart_write_bytes(UNUM, p, n);
            if (w <= 0)
                break;
            s_st.tx_bytes += (uint32_t)w;
            portinfo_tx_bytes(1, (size_t)w);
            xSemaphoreTake(s_mux, portMAX_DELAY);
            s_tail = (s_tail + (size_t)w) % TX_RING;
            xSemaphoreGive(s_mux);
        }
    }
}

static void rx_task(void *arg)
{
    static uint8_t buf[512];
    uart_event_t ev;
    for (;;) {
        if (xQueueReceive(s_evq, &ev, LINE_TICKS(LONG_IDLE_MS)) != pdTRUE) {
            relay_line_idle(1, true);
            continue;
        }
        if (ev.type == UART_DATA) {
            int n; /* everything buffered, not just ev.size: see link.c */
            while ((n = uart_read_bytes(UNUM, buf, sizeof buf, 0)) > 0) {
                s_st.rx_bytes += (uint32_t)n;
                portinfo_rx_bytes(1, (size_t)n);
                relay_line_bytes(1, buf, (size_t)n);
            }
            if (ev.timeout_flag)
                relay_line_idle(1, false);
        } else if (ev.type == UART_FIFO_OVF || ev.type == UART_BUFFER_FULL) {
            xQueueReset(s_evq);
            uart_flush_input(UNUM);
            s_st.rx_overflows++;
        }
    }
}

bool uline_start(const line_cfg_t *c)
{
    if (s_run)
        return true;
    if (c->tx_pin < 0 || c->rx_pin < 0) {
        ESP_LOGI(TAG, "line 1 pins not configured: UART1 stays off");
        return false;
    }
    const uart_config_t cfg = {
        .baud_rate = (int)c->baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    s_ring = malloc(TX_RING);
    s_mux = xSemaphoreCreateMutex();
    if (!s_ring || !s_mux || uart_driver_install(UNUM, RX_RING, DRV_TX_RING, 32, &s_evq, 0) != ESP_OK ||
        uart_param_config(UNUM, &cfg) != ESP_OK ||
        uart_set_pin(UNUM, c->tx_pin, c->rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        ESP_LOGE(TAG, "UART1 on TX %d / RX %d failed", c->tx_pin, c->rx_pin);
        return false;
    }
    /* IDF 5.5 no longer pulls a matrix-routed RX pin up: with nothing plugged into X2 the input would
     * float and its noise would be relayed as data. The weak pull-up keeps an open line idle (high). */
    /* RX FIFO interrupt at 64 of 128 bytes instead of the default 120: at 5 Mbaud the default left 16 us for
     * the ISR, less than a Wi-Fi interrupt can take (no measurement; the counter is rx_overflows). */
    uart_set_rx_full_threshold(UNUM, 64);
    gpio_pullup_en(c->rx_pin);
    s_tx_pin = c->tx_pin;
    s_rx_pin = c->rx_pin;
    s_baud = c->baud;
    s_switched_us = esp_timer_get_time();
    s_run = true;
    xTaskCreate(tx_task, "uline_tx", 2560, NULL, 11, &s_tx);
    xTaskCreate(rx_task, "uline_rx", 3584, NULL, 12, NULL);
    ESP_LOGI(TAG, "line 1: UART1 TX %d RX %d @ %lu", c->tx_pin, c->rx_pin, (unsigned long)c->baud);
    return true;
}

uint32_t uline_baud(void)
{
    return s_pending_baud ? s_pending_baud : s_baud;
}

int64_t uline_baud_switched_us(void)
{
    return s_pending_baud ? 0 : s_switched_us;
}

void uline_get_stats(uline_stats_t *s)
{
    memcpy(s, (const void *)&s_st, sizeof *s);
}

void uline_pins(int8_t *tx, int8_t *rx)
{
    *tx = s_tx_pin;
    *rx = s_rx_pin;
}
