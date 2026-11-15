/* The heater's own record on the box (firmware 1.70, docs/API_SPEC.md §9.8): saved in NVS, read again by the box
 * itself at power-up and around every run, and the preheat's coolant stop taken from the heater's own setting. */
#include <stdlib.h>
#include <string.h>
#include "at_app.h"
#include "at_hrec.h"
#include "at_hrec_json.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "hrec";
#define KEY_HREC "heater_rec"

/* Reads the box asks for by itself: once after power-up, AFTER_START after the heater leaves idle (the start is
 * counted by then), AFTER_STOP after it is idle again (the run's hours are in). */
#define BOOT_MIN_MS    30000u
#define BOOT_CLOCK_MS  180000u   /* wait this long for the clock, so a first read can date its faults */
#define AFTER_START_MS 120000u
#define AFTER_STOP_MS  20000u
#define RETRY_MS       15000u
#define MAX_TRIES      8
/* 1.72: a read that finished with a report page missing (no reply, or a reply that was not ours) is asked again
 * this soon, up to this many times, so the record is never left stale after a Fetch */
#define INCOMPLETE_RETRY_MS 3000u
#define INCOMPLETE_MAX      3

static at_hrec_t s_rec;
static SemaphoreHandle_t s_lock;
static bool s_boot_done, s_due;
static uint32_t s_due_ms;
static int s_tries;
static int s_prev = -1;          /* 0 idle, 1 running or cooling down, -1 not known */
static uint32_t s_taken_read, s_taken_set, s_taken_ver;
static int s_incomplete;         /* retries made for the current read that came back short */
static volatile bool s_retrying; /* a retry is waiting: the status keeps "reading" true meanwhile */
static uint32_t s_retry_since;
#define RETRYING_MAX_MS 30000u   /* never show "reading" longer than this for a retry that cannot be sent */

void at_app_hrec_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    at_hrec_init(&s_rec);
    at_hrec_t *t = malloc(sizeof(*t));
    if (t && at_settings_blob_load(KEY_HREC, t, sizeof(*t)) && t->version == AT_HREC_VERSION) {
        s_rec = *t;
        ESP_LOGI(TAG, "heater record loaded (have 0x%02x, read at %lu)", s_rec.have, (unsigned long)s_rec.read_at);
    }
    free(t);
}

static void save(const at_hrec_t *r) { at_settings_blob_save(KEY_HREC, r, sizeof(*r)); }

void at_app_hrec_copy(at_hrec_t *out)
{
    if (!s_lock) {
        at_hrec_init(out);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_rec;
    xSemaphoreGive(s_lock);
}

float at_app_preheat_stop_temp(float def)
{
    if (!s_lock) return def;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    float t = at_hrec_preheat_temp(&s_rec, def);
    xSemaphoreGive(s_lock);
    return t;
}

static void schedule(uint32_t now, uint32_t after)
{
    s_due = true;
    s_due_ms = now + after;
    s_tries = 0;
}

void at_app_hrec_tick(const at_snapshot_t *snap, uint32_t now, bool clock_ok, int64_t epoch, bool bypass)
{
    if (!s_lock) return;
    at_heater_info_t *hi = malloc(sizeof(*hi));
    if (!hi) return;
    uint32_t n;
    at_app_heater_info(hi, &n);

    /* take what a finished read (or the controller's own read at power-up) brought in */
    if (hi->pending <= 0 && ((hi->have_read && hi->read_ms != s_taken_read) ||
                             (hi->have_settings && hi->settings_ms != s_taken_set) ||
                             (hi->have_version && hi->version_ms != s_taken_ver))) {
        const bool new_read = hi->have_read && hi->read_ms != s_taken_read;
        s_taken_read = hi->read_ms;
        s_taken_set = hi->settings_ms;
        s_taken_ver = hi->version_ms;
        at_hrec_t *copy = malloc(sizeof(*copy));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool changed = at_hrec_take(&s_rec, hi, clock_ok && epoch > 0 ? (uint32_t)epoch : 0);
        if (copy) *copy = s_rec;
        xSemaphoreGive(s_lock);
        if (changed && copy) save(copy);
        free(copy);
        if (new_read) {
            if (hi->pages == (1u << AT_EEPROM_PAGES) - 1) {
                s_incomplete = 0;
                s_retrying = false;
            } else if (s_incomplete < INCOMPLETE_MAX) {
                s_incomplete++;
                ESP_LOGW(TAG, "heater read came back without the whole report (pages 0x%x): asking again (%d of %d)",
                         hi->pages, s_incomplete, INCOMPLETE_MAX);
                if (!s_due) schedule(now, INCOMPLETE_RETRY_MS);
                s_retrying = true;
                s_retry_since = now;
            } else {
                ESP_LOGW(TAG, "heater report still incomplete after %d retries: keeping the last whole one",
                         INCOMPLETE_MAX);
                s_incomplete = 0;
                s_retrying = false;
            }
        }
    }

    if (s_retrying && now - s_retry_since > RETRYING_MAX_MS) s_retrying = false;

    bool fresh = snap->have_status && !snap->stale;
    if (fresh) {
        int cls = snap->status.state_raw == 0x00 ? 0 : (snap->status.state_raw >= 0x02 && snap->status.state_raw <= 0x04) ? 1 : -1;
        if (cls >= 0) {
            if (s_prev == 0 && cls == 1) schedule(now, AFTER_START_MS);
            if (s_prev == 1 && cls == 0) schedule(now, AFTER_STOP_MS);
            s_prev = cls;
        }
        if (!s_boot_done && now >= BOOT_MIN_MS && (clock_ok || now >= BOOT_CLOCK_MS)) {
            s_boot_done = true;
            if (!s_due) schedule(now, 0);
        }
    }
    if (s_due && fresh && !bypass && (int32_t)(now - s_due_ms) >= 0 && hi->pending <= 0) {
        if (at_app_read_heater_info()) {
            s_due = false;
        } else if (++s_tries >= MAX_TRIES) {
            s_due = false;
            s_retrying = false;
        } else {
            s_due_ms = now + RETRY_MS;
        }
    }
    free(hi);
}

bool at_app_hrec_retrying(void) { return s_retrying; }

cJSON *at_app_hrec_backup(void)
{
    at_hrec_t *r = malloc(sizeof(*r));
    if (!r) return NULL;
    at_app_hrec_copy(r);
    cJSON *o = at_hrec_to_json(r);
    free(r);
    return o;
}

bool at_app_hrec_restore_json(const cJSON *o)
{
    if (!s_lock) return false;
    at_hrec_t *b = malloc(sizeof(*b)), *copy = malloc(sizeof(*copy));
    bool ok = b && copy && at_hrec_from_json(o, b);
    if (ok) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool changed = at_hrec_restore(&s_rec, b);
        *copy = s_rec;
        xSemaphoreGive(s_lock);
        if (changed) save(copy);
    }
    free(b);
    free(copy);
    return ok;
}
