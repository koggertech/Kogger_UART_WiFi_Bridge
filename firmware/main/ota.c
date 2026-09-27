#include "ota.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "sbpdev.h"

static const char *TAG = "ota";

#define WINDOW_US        (5LL * 1000 * 1000)    /* same window a Kogger bootloader gives the host */
#define IDLE_US          (30LL * 1000 * 1000)   /* transfer stalled: give up, keep running */
/* A new image confirms itself only after a minute of work with the host talking: a release that dies
 * on connecting, on the first relayed traffic or on a client joining still gets rolled back. */
#define CONFIRM_MIN_US   (60LL * 1000 * 1000)
#define CONFIRM_MAX_US   ((int64_t)CONFIG_WB_OTA_CONFIRM_S * 1000 * 1000)
#define HDR_LEN          256                    /* image header + first segment header + app descriptor */
#define DESC_OFFSET      (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))

typedef enum { ST_IDLE, ST_WINDOW, ST_RECEIVING } st_t;

static st_t s_st;
static int64_t s_deadline;
static const esp_partition_t *s_part;
static esp_ota_handle_t s_h;
static bool s_open;
static uint16_t s_last;          /* last stored packet number */
static uint32_t s_off;           /* bytes stored */
static bool s_aborted;           /* the last transfer was abandoned: "run firmware" must not say OK */
static uint8_t s_hdr[HDR_LEN];
static uint32_t s_hdr_len;
static bool s_hdr_ok;

static bool s_pending;           /* this boot runs an image that still has to confirm itself */
static bool s_host_seen;
static esp_ota_img_states_t s_state = ESP_OTA_IMG_UNDEFINED;

void ota_init(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(run, &s_state) != ESP_OK)
        s_state = ESP_OTA_IMG_UNDEFINED;
    s_pending = s_state == ESP_OTA_IMG_PENDING_VERIFY;
    ESP_LOGI(TAG, "running %s, state %s", run ? run->label : "?", ota_state_name());
}

uint8_t ota_boot_mode(void)
{
    return s_st == ST_IDLE ? 0 : 1;
}

const char *ota_running_label(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    return run ? run->label : "?";
}

const char *ota_state_name(void)
{
    switch (s_state) {
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending";
    case ESP_OTA_IMG_VALID:          return "valid";
    case ESP_OTA_IMG_NEW:            return "new";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "aborted";
    default:                         return "undefined";
    }
}

/* ---- session -------------------------------------------------------------------------------- */

static void session_close(void)
{
    if (s_open) {
        esp_ota_abort(s_h);
        s_open = false;
    }
    s_st = ST_IDLE;
}

static void progress(uint8_t type, uint16_t rcv)
{
    uint8_t p[8];
    sbp_put_u16(p, s_last);
    sbp_put_u32(p + 2, s_off);
    p[6] = type;
    p[7] = (uint8_t)rcv;
    sbpdev_send(SBP_T_CONTENT, 0, SBP_ID_UPDATE, p, sizeof p);
}

static void fatal(uint8_t type, uint16_t rcv)
{
    ESP_LOGW(TAG, "update aborted: type %u at packet %u, %lu bytes", type, rcv, (unsigned long)s_off);
    session_close();
    s_aborted = true;
    progress(type, rcv);
}

void ota_boot_request(void)
{
    session_close();              /* a new ID_BOOT v0 restarts from scratch */
    s_last = 0;                   /* a reposition inside the window must not point into an old session */
    s_off = 0;
    s_aborted = false;
    s_st = ST_WINDOW;
    s_deadline = esp_timer_get_time() + WINDOW_US;
    sbpdev_clear_mark();          /* as after a real reset: the host re-establishes its session */
    ESP_LOGI(TAG, "update window open");
}

/* The first HDR_LEN bytes are held back until they prove to be an image of this firmware for this
 * chip; only then is the slot touched. */
static bool header_ok(void)
{
    const esp_image_header_t *h = (const esp_image_header_t *)s_hdr;
    const esp_app_desc_t *d = (const esp_app_desc_t *)(s_hdr + DESC_OFFSET);
    const esp_app_desc_t *me = esp_app_get_description();
    if (h->magic != ESP_IMAGE_HEADER_MAGIC || h->chip_id != ESP_CHIP_ID_ESP32C3) {
        ESP_LOGW(TAG, "not an ESP32-C3 image (magic %02x chip %u)", h->magic, (unsigned)h->chip_id);
        return false;
    }
    if (d->magic_word != ESP_APP_DESC_MAGIC_WORD || strncmp(d->project_name, me->project_name, sizeof d->project_name)) {
        ESP_LOGW(TAG, "image of another project");
        return false;
    }
    ESP_LOGI(TAG, "incoming image %.32s %.32s", d->project_name, d->version);
    return true;
}

static uint8_t consume(const uint8_t *data, uint32_t n)
{
    if (!s_hdr_ok) {
        uint32_t take = HDR_LEN - s_hdr_len < n ? HDR_LEN - s_hdr_len : n;
        memcpy(s_hdr + s_hdr_len, data, take);
        s_hdr_len += take;
        data += take;
        n -= take;
        if (s_hdr_len < HDR_LEN)
            return OTA_T_ACCEPTED;
        if (!header_ok())
            return OTA_T_BAD_IMAGE;
        if (esp_ota_begin(s_part, OTA_WITH_SEQUENTIAL_WRITES, &s_h) != ESP_OK)
            return OTA_T_FLASH;
        s_open = true;
        s_hdr_ok = true;
        if (esp_ota_write(s_h, s_hdr, HDR_LEN) != ESP_OK)
            return OTA_T_FLASH;
    }
    if (n && esp_ota_write(s_h, data, n) != ESP_OK)
        return OTA_T_FLASH;
    return OTA_T_ACCEPTED;
}

void ota_on_update(const sbp_frame_t *f)
{
    if (sbp_type(f->mode) != SBP_T_SETTING || sbp_ver(f->mode) != 0) {
        sbpdev_ack(f, SBP_RESP_ERR_VERSION);
        return;
    }
    uint16_t num = f->len >= 2 ? sbp_get_u16(f->payload) : 0;
    if (f->len < 3 || s_st == ST_IDLE) {
        progress(OTA_T_NO_SESSION, num); /* fatal for the host: it stops instead of streaming blind */
        return;
    }
    if (s_st == ST_WINDOW) {
        if (num != 1) {
            progress(OTA_T_REPOSITION, num);
            return;
        }
        s_part = esp_ota_get_next_update_partition(NULL);
        if (!s_part) {
            fatal(OTA_T_FLASH, num);
            return;
        }
        s_last = 0;
        s_off = 0;
        s_hdr_len = 0;
        s_hdr_ok = false;
        s_st = ST_RECEIVING;
        ESP_LOGI(TAG, "receiving into %s (%lu bytes max)", s_part->label, (unsigned long)s_part->size);
    }
    s_deadline = esp_timer_get_time() + IDLE_US;

    if (num != (uint16_t)(s_last + 1)) {
        /* a resend of a stored chunk, or one after a lost chunk: tell the host where we are */
        progress(OTA_T_REPOSITION, num);
        return;
    }
    uint32_t n = f->len - 2u;
    if (s_off + n > s_part->size) {
        fatal(OTA_T_TOO_BIG, num);
        return;
    }
    uint8_t t = consume(f->payload + 2, n);
    if (t != OTA_T_ACCEPTED) {
        fatal(t, num);
        return;
    }
    s_last = num;
    s_off += n;
    progress(OTA_T_ACCEPTED, num);
}

uint8_t ota_run_request(bool *reboot)
{
    *reboot = false;
    if (s_st == ST_WINDOW) {
        s_st = ST_IDLE;           /* "run firmware" with nothing received: just close the window */
        return SBP_RESP_OK;
    }
    if (s_st != ST_RECEIVING)
        return s_aborted ? SBP_RESP_ERR_RUNTIME : SBP_RESP_OK; /* a dropped transfer is not "accepted" */
    if (!s_hdr_ok || !s_open) {
        session_close();
        return SBP_RESP_ERR_RUNTIME;
    }
    /* Checks the whole image (segments, appended SHA-256) before the slot can be booted. */
    esp_err_t e = esp_ota_end(s_h);
    s_open = false;
    s_st = ST_IDLE;
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "image rejected: %s (%lu bytes)", esp_err_to_name(e), (unsigned long)s_off);
        return SBP_RESP_ERR_RUNTIME;
    }
    e = esp_ota_set_boot_partition(s_part);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "cannot select %s: %s", s_part->label, esp_err_to_name(e));
        return SBP_RESP_ERR_RUNTIME;
    }
    ESP_LOGI(TAG, "image OK (%lu bytes), next boot from %s", (unsigned long)s_off, s_part->label);
    *reboot = true;
    return SBP_RESP_OK;
}

void ota_reboot_into_new(void)
{
    sbpdev_save_resume();         /* same baud and address after the reboot: no false rollback */
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

/* ---- confirmation of a freshly updated image ------------------------------------------------- */

void ota_note_host_frame(void)
{
    s_host_seen = true;
}

void ota_tick(void)
{
    int64_t now = esp_timer_get_time();
    if (s_st == ST_WINDOW && now > s_deadline) {
        ESP_LOGI(TAG, "no image in the window: reboot");
        esp_restart();            /* what a bootloader does when nothing arrives: run the firmware */
    }
    if (s_st == ST_RECEIVING && now > s_deadline) {
        ESP_LOGW(TAG, "transfer stalled at %lu bytes: abandoned", (unsigned long)s_off);
        session_close();
        s_aborted = true;
    }
    if (!s_pending)
        return;
#if CONFIG_WB_OTA_TEST_NO_CONFIRM
    const bool may_confirm = false; /* test build: must be rolled back */
#else
    const bool may_confirm = true;
#endif
    if (may_confirm && s_host_seen && now >= CONFIRM_MIN_US) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            s_pending = false;
            s_state = ESP_OTA_IMG_VALID;
            ESP_LOGI(TAG, "new image confirmed");
        }
    } else if (now > CONFIRM_MAX_US) {
        ESP_LOGW(TAG, "new image never heard the host: reboot, the bootloader rolls back");
        esp_restart();
    }
}
