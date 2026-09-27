#include "link.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "frame.h"
#include "relay.h"

#if CONFIG_WB_LINK_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
/* Anything else printing to USB Serial/JTAG would corrupt the frame stream. */
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG
#error "Link uses USB Serial/JTAG: move the console to UART0 and disable the secondary console (see sdkconfig.defaults)"
#endif
#else
#include "driver/gpio.h"
#include "driver/uart.h"
#if CONFIG_ESP_CONSOLE_UART && (CONFIG_ESP_CONSOLE_UART_NUM == CONFIG_WB_LINK_UART_NUM)
#error "Link UART is also the console UART: use sdkconfig.defaults.uart (console on USB Serial/JTAG)"
#endif
#endif

static const char *TAG = "link";

/* Buffers (docs/RELAY.md): UART driver RX ring and our TX ring of whole units. The UART driver gets
 * no TX ring: with one, IDF 5.5.5 uart_write_bytes() polls a full ring without ever blocking
 * (uart.c uart_tx_all), and this priority-11 task starved everything below it until the task
 * watchdog rebooted the module (field, 2026-09-26). Without it the call sleeps on the FIFO. */
#define UART_RX_RING  16384   /* 40 ms at 4 Mbaud, 180 ms at 921600 */
#define UART_TX_RING  0
#define USB_TX_RING   4096    /* USB Serial/JTAG driver ring; also the largest write it accepts */
#define TX_RING       12288   /* whole frames waiting for the transport */
#define TX_RESERVE    1024    /* kept for the module's own frames: relayed data never takes it */
#define LONG_IDLE_MS  20      /* no byte for this long: release an unfinished frame candidate */

static link_rx_handler_t s_on_ip, s_on_ctl;
static link_sbp_handler_t s_on_sbp;
static frame_dec_t s_dec;
static uint8_t s_decbuf[FRAME_MAX_PAYLOAD + 3];
static sbp_dec_t s_sbp;
static volatile link_proto_t s_proto = LINK_PROTO_NONE;
static volatile uint32_t s_baud, s_pending_baud;
static volatile uint32_t s_tx_frames, s_tx_dropped, s_rx_overflows;

/* TX ring: writers copy whole units under the mutex, the TX task drains it into the transport. */
static uint8_t *s_ring;
static size_t s_head, s_tail;          /* write / read positions, one slot kept empty */
static size_t s_baud_mark;             /* s_head when the pending rate change was requested */
static SemaphoreHandle_t s_rmux;
static TaskHandle_t s_txtask;

/* pdMS_TO_TICKS() rounds down: with CONFIG_FREERTOS_HZ=100 anything under 10 ms becomes 0, i.e.
 * a non-blocking call. A 0-tick read in rx_task once turned it into a busy loop that starved every
 * lower-priority task (docs/DESIGN.md, bench log 2026-09-25), so every wait goes through this. */
#define LINK_TICKS(ms) (pdMS_TO_TICKS(ms) > 0 ? pdMS_TO_TICKS(ms) : 1)

#if CONFIG_WB_LINK_UART
static QueueHandle_t s_uart_evq;
#endif

static int transport_write(const uint8_t *buf, size_t len)
{
#if CONFIG_WB_LINK_USB_SERIAL_JTAG
    /* xRingbufferSend() refuses an item larger than the ring outright: write at most one ring's worth. */
    return usb_serial_jtag_write_bytes(buf, len < USB_TX_RING ? len : USB_TX_RING, LINK_TICKS(100));
#else
    return uart_write_bytes(CONFIG_WB_LINK_UART_NUM, buf, len); /* sleeps on the FIFO until all is in */
#endif
}

static void transport_set_baud(uint32_t baud)
{
#if CONFIG_WB_LINK_UART
    /* Everything queued before the request (typically the acknowledgement) leaves at the old rate. */
    uart_wait_tx_done(CONFIG_WB_LINK_UART_NUM, LINK_TICKS(500));
    uart_set_baudrate(CONFIG_WB_LINK_UART_NUM, baud);
    uint32_t real = 0;
    uart_get_baudrate(CONFIG_WB_LINK_UART_NUM, &real);
    ESP_LOGI(TAG, "baud %lu (real %lu)", (unsigned long)baud, (unsigned long)real);
#endif
    s_baud = baud;
}

static void transport_init(void)
{
#if CONFIG_WB_LINK_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t cfg = {
        .rx_buffer_size = 8192,
        .tx_buffer_size = USB_TX_RING,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    s_baud = 921600;
    ESP_LOGI(TAG, "transport: USB Serial/JTAG");
#else
    const uart_config_t cfg = {
        .baud_rate = CONFIG_WB_LINK_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = (CONFIG_WB_LINK_UART_RTS >= 0 && CONFIG_WB_LINK_UART_CTS >= 0)
                         ? UART_HW_FLOWCTRL_CTS_RTS : UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 100,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(CONFIG_WB_LINK_UART_NUM, UART_RX_RING, UART_TX_RING, 32, &s_uart_evq, 0));
    ESP_ERROR_CHECK(uart_param_config(CONFIG_WB_LINK_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(CONFIG_WB_LINK_UART_NUM, CONFIG_WB_LINK_UART_TX, CONFIG_WB_LINK_UART_RX,
                                 CONFIG_WB_LINK_UART_RTS, CONFIG_WB_LINK_UART_CTS));
    gpio_pullup_en(CONFIG_WB_LINK_UART_RX); /* an unplugged connector idles high instead of floating */
    s_baud = CONFIG_WB_LINK_UART_BAUD;
    ESP_LOGI(TAG, "transport: UART%d @ %d baud", CONFIG_WB_LINK_UART_NUM, CONFIG_WB_LINK_UART_BAUD);
#endif
}

/* ---- TX ring ---------------------------------------------------------------------------------- */

static size_t ring_used(void)
{
    return (s_head + TX_RING - s_tail) % TX_RING;
}

static bool tx_put(const uint8_t *d, size_t n, size_t reserve)
{
    if (n == 0)
        return true;
    xSemaphoreTake(s_rmux, portMAX_DELAY);
    bool fits = n + reserve <= TX_RING - 1 - ring_used();
    if (fits) {
        size_t first = TX_RING - s_head < n ? TX_RING - s_head : n;
        memcpy(s_ring + s_head, d, first);
        memcpy(s_ring, d + first, n - first);
        s_head = (s_head + n) % TX_RING;
    }
    xSemaphoreGive(s_rmux);
    if (!fits) {
        s_tx_dropped++; /* all or nothing: the reader never sees a piece of a frame */
        return false;
    }
    s_tx_frames++;
    xTaskNotifyGive(s_txtask);
    return true;
}

bool link_tx_frame(const uint8_t *d, size_t n)
{
    return tx_put(d, n, 0);
}

bool link_tx_relay(const uint8_t *d, size_t n)
{
    return tx_put(d, n, TX_RESERVE);
}

static void tx_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, LINK_TICKS(100));
        int stalls = 0;
        for (;;) {
            xSemaphoreTake(s_rmux, portMAX_DELAY);
            bool pend = s_pending_baud != 0;
            size_t end = pend ? s_baud_mark : s_head; /* before a rate change: send only up to its mark */
            size_t n = end >= s_tail ? end - s_tail : TX_RING - s_tail; /* contiguous part */
            const uint8_t *p = s_ring + s_tail;
            xSemaphoreGive(s_rmux);
            if (n == 0) {
                if (pend) { /* everything queued before the request is out: switch, then go on */
                    xSemaphoreTake(s_rmux, portMAX_DELAY);
                    uint32_t b = s_pending_baud;
                    xSemaphoreGive(s_rmux);
                    transport_set_baud(b);
                    xSemaphoreTake(s_rmux, portMAX_DELAY);
                    if (s_pending_baud == b)
                        s_pending_baud = 0; /* a newer request keeps its own mark and runs next */
                    xSemaphoreGive(s_rmux);
                    continue;
                }
                break;
            }
            int w = transport_write(p, n);
            if (w <= 0) {
                if (++stalls >= 5)
                    break; /* host not reading (USB closed): keep the data, writers drop new units */
                continue;
            }
            stalls = 0;
            xSemaphoreTake(s_rmux, portMAX_DELAY);
            s_tail = (s_tail + (size_t)w) % TX_RING;
            xSemaphoreGive(s_rmux);
        }
    }
}

/* ---- RX ----------------------------------------------------------------------------------------- */

static void on_frame(void *ctx, uint8_t type, const uint8_t *p, size_t n)
{
    (void)ctx;
    if (s_proto == LINK_PROTO_SBP)
        return;
    s_proto = LINK_PROTO_SLIP;
    if (type == FRAME_T_IP && s_on_ip)
        s_on_ip(p, n);
    else if (type == FRAME_T_CTL && s_on_ctl)
        s_on_ctl(p, n);
}

static void on_sbp(void *ctx, const sbp_frame_t *f)
{
    (void)ctx;
    if (s_proto != LINK_PROTO_SLIP && s_on_sbp)
        s_on_sbp(f); /* the handler locks the link to SBP when the frame is for us */
}

static void feed(const uint8_t *buf, size_t n)
{
    if (relay_active()) {
        relay_uart_bytes(buf, n); /* whole frames: ours to the manager, the rest to the boat */
        return;
    }
    frame_dec_feed(&s_dec, buf, n, on_frame, NULL);
    sbp_dec_feed(&s_sbp, buf, n, on_sbp, NULL);
}

static void idle(bool long_idle)
{
    if (relay_active())
        relay_uart_idle(long_idle);
}

static void rx_task(void *arg)
{
    static uint8_t buf[512];
#if CONFIG_WB_LINK_USB_SERIAL_JTAG
    for (;;) {
        /* Returns as soon as any data is buffered (ring-buffer "receive up to"). */
        int n = usb_serial_jtag_read_bytes(buf, sizeof buf, LINK_TICKS(LONG_IDLE_MS));
        if (n > 0) {
            feed(buf, (size_t)n);
            idle(false);          /* USB delivers whole host writes: treat each read as a burst end */
        } else {
            idle(true);
        }
    }
#else
    /* Sleep on driver events: UART_DATA arrives on RX timeout (a few symbol times of silence) or
     * when the FIFO threshold is reached, so latency stays far below one tick. */
    uart_event_t ev;
    for (;;) {
        if (xQueueReceive(s_uart_evq, &ev, LINK_TICKS(LONG_IDLE_MS)) != pdTRUE) {
            idle(true);
            continue;
        }
        if (ev.type == UART_DATA) {
            /* Everything buffered, not just ev.size: the driver drops events when the queue is full
             * and keeps their bytes, which would then wait for the next burst. */
            int n;
            while ((n = uart_read_bytes(CONFIG_WB_LINK_UART_NUM, buf, sizeof buf, 0)) > 0)
                feed(buf, (size_t)n);
            if (ev.timeout_flag)
                idle(false);      /* line quiet for a few symbols: end of a burst */
        } else if (ev.type == UART_FIFO_OVF || ev.type == UART_BUFFER_FULL) {
            /* Bytes were lost; the half-received frame fails its check and is dropped by the decoder.
             * Queue first: events arriving after the flush re-enables reception must survive. */
            xQueueReset(s_uart_evq);
            uart_flush_input(CONFIG_WB_LINK_UART_NUM);
            s_rx_overflows++;
        }
    }
#endif
}

/* ---- public ------------------------------------------------------------------------------------- */

void link_start(link_rx_handler_t on_ip, link_rx_handler_t on_ctl, link_sbp_handler_t on_sbp)
{
    s_on_ip = on_ip;
    s_on_ctl = on_ctl;
    s_on_sbp = on_sbp;
    frame_dec_init(&s_dec, s_decbuf, sizeof s_decbuf);
    sbp_dec_init(&s_sbp);
    s_ring = malloc(TX_RING);
    s_rmux = xSemaphoreCreateMutex();
    configASSERT(s_ring && s_rmux);
    transport_init();
    xTaskCreate(tx_task, "link_tx", 3072, NULL, 11, &s_txtask);
    xTaskCreate(rx_task, "link_rx", 4096, NULL, 12, NULL);
}

link_proto_t link_proto(void)
{
    return s_proto;
}

void link_set_proto(link_proto_t p)
{
    if (p != s_proto)
        ESP_LOGI(TAG, "protocol %d -> %d", (int)s_proto, (int)p);
    s_proto = p;
}

void link_send(uint8_t type, const uint8_t *payload, size_t len)
{
    if (s_proto == LINK_PROTO_SBP)
        return; /* nobody on the other end reads SLIP: not a loss */
    if (len > FRAME_MAX_PAYLOAD) {
        s_tx_dropped++;
        return;
    }
    size_t cap = FRAME_ENCODED_MAX(len);
    uint8_t *b = malloc(cap);
    if (!b) {
        s_tx_dropped++;
        return;
    }
    link_tx_frame(b, frame_encode(type, payload, len, b, cap));
    free(b);
}

void link_send_sbp(uint8_t route, uint8_t mode, uint8_t id, const uint8_t *payload, uint8_t len)
{
    if (s_proto != LINK_PROTO_SBP)
        return;
    uint8_t f[SBP_FRAME_MAX];
    link_tx_frame(f, sbp_encode(route, mode, id, payload, len, f));
}

bool link_set_baud(uint32_t baud)
{
    if (baud < LINK_BAUD_MIN || baud > LINK_BAUD_MAX)
        return false;
    xSemaphoreTake(s_rmux, portMAX_DELAY);
    s_baud_mark = s_head; /* applied by the TX task once everything queued so far has left */
    s_pending_baud = baud;
    xSemaphoreGive(s_rmux);
    xTaskNotifyGive(s_txtask);
    return true;
}

uint32_t link_baud(void)
{
    return s_pending_baud ? s_pending_baud : s_baud;
}

void link_get_stats(link_stats_t *out)
{
    out->rx_frames = s_dec.frames_ok;
    out->rx_crc_errors = s_dec.crc_errors;
    out->rx_discarded = s_dec.discarded;
    out->sbp_rx_frames = s_sbp.frames_ok;
    out->sbp_check_errors = s_sbp.check_errors;
    out->tx_frames = s_tx_frames;
    out->tx_dropped = s_tx_dropped;
    out->rx_overflows = s_rx_overflows;
}
