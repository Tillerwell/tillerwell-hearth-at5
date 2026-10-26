/*
 * Heater timers on the box: the saved schedule, the clock and time zone it runs on, and the task that starts
 * runs. The rules and the firing logic are in at_core/at_timers.c (host-tested); this file supplies local time,
 * storage and the real heater commands. See docs/TIMERS.md.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "at_app.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "timers";

#define NVS_NS        "autoterm"
#define KEY_TIMERS    "timers"
#define KEY_TZ        "tz"
#define STORE_VERSION 1
#define MIN_EPOCH     1704067200LL   /* 2024-01-01: anything earlier is a clock that was never set */
#define MAX_EPOCH     4102444800LL   /* 2100-01-01 */

typedef struct {
    uint32_t version;
    uint32_t item_size;
    uint32_t count;
} store_hdr_t;

static SemaphoreHandle_t s_lock;     /* guards everything below; taken before the thermostat and core locks */
static at_timer_t s_list[AT_TIMERS_MAX];
static int s_n;
static uint32_t s_version;
static at_runner_t s_runner;
static char s_tz[AT_TZ_MAX];
static volatile bool s_time_ok;      /* the system clock holds real time */
static volatile bool s_sntp_synced;  /* ... from the network this boot */
static bool s_sntp_started;
static bool s_started;

/* ---- storage ---------------------------------------------------------------------------------------- */

static esp_err_t save_list_locked(const at_timer_t *list, int n)
{
    size_t size = sizeof(store_hdr_t) + (size_t)n * sizeof(at_timer_t);
    uint8_t *buf = malloc(size);
    if (!buf) return ESP_ERR_NO_MEM;
    store_hdr_t hdr = {STORE_VERSION, sizeof(at_timer_t), (uint32_t)n};
    memcpy(buf, &hdr, sizeof(hdr));
    if (n) memcpy(buf + sizeof(hdr), list, (size_t)n * sizeof(at_timer_t));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, KEY_TIMERS, buf, size);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    free(buf);
    if (err != ESP_OK) ESP_LOGE(TAG, "saving timers failed: %s", esp_err_to_name(err));
    return err;
}

static void load_list(void)
{
    s_n = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t size = 0;
    if (nvs_get_blob(h, KEY_TIMERS, NULL, &size) == ESP_OK && size >= sizeof(store_hdr_t) &&
        size <= sizeof(store_hdr_t) + AT_TIMERS_MAX * sizeof(at_timer_t)) {
        uint8_t *buf = malloc(size);
        if (buf && nvs_get_blob(h, KEY_TIMERS, buf, &size) == ESP_OK) {
            store_hdr_t hdr;
            memcpy(&hdr, buf, sizeof(hdr));
            if (hdr.version == STORE_VERSION && hdr.item_size == sizeof(at_timer_t) && hdr.count <= AT_TIMERS_MAX &&
                size == sizeof(hdr) + hdr.count * sizeof(at_timer_t)) {
                at_timer_t tmp[AT_TIMERS_MAX];
                memcpy(tmp, buf + sizeof(hdr), hdr.count * sizeof(at_timer_t));
                at_localtime_t noclock = {0};   /* nothing is "in the past" without a clock */
                at_timer_problem_t p = at_timers_validate(tmp, (int)hdr.count, NULL, 0, &noclock, 0);
                if (p.err == AT_TERR_NONE) {
                    memcpy(s_list, tmp, hdr.count * sizeof(at_timer_t));
                    s_n = (int)hdr.count;
                } else {
                    ESP_LOGE(TAG, "saved timers rejected (%s), starting empty", at_timer_err_text(p.err));
                }
            } else {
                ESP_LOGW(TAG, "saved timers have an unknown layout, starting empty");
            }
        }
        free(buf);
    }
    nvs_close(h);
}

/* ---- clock and zone --------------------------------------------------------------------------------- */

static void apply_tz(const char *posix)
{
    setenv("TZ", posix, 1);
    tzset();
}

static bool time_plausible(void)
{
    return time(NULL) > MIN_EPOCH;
}

static void local_now(at_localtime_t *out)
{
    memset(out, 0, sizeof(*out));
    time_t t = time(NULL);
    if (!s_tz[0] || !s_time_ok || t < MIN_EPOCH) return;
    struct tm tm;
    localtime_r(&t, &tm);
    out->ok = true;
    out->epoch_s = t;
    out->day = at_days_from_civil(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    out->minute = (uint16_t)(tm.tm_hour * 60 + tm.tm_min);
}

static int64_t local_minute_to_epoch(int64_t lm)
{
    int y, m, d;
    at_civil_from_days((int32_t)(lm / 1440), &y, &m, &d);
    int mi = (int)(lm % 1440);
    struct tm tm = {0};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_hour = mi / 60;
    tm.tm_min = mi % 60;
    tm.tm_isdst = -1;
    return (int64_t)mktime(&tm);
}

static void sntp_synced_cb(struct timeval *tv)
{
    s_time_ok = true;
    s_sntp_synced = true;
    ESP_LOGI(TAG, "network time received");
}

void at_app_clock_sntp_start(void)
{
    if (s_sntp_started) return;
    s_sntp_started = true;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(sntp_synced_cb);
    esp_sntp_init();
}

bool at_app_set_time(int64_t epoch_s)
{
    if (s_sntp_synced || epoch_s < MIN_EPOCH || epoch_s > MAX_EPOCH) return false;
    struct timeval tv = {.tv_sec = (time_t)epoch_s, .tv_usec = 0};
    if (settimeofday(&tv, NULL) != 0) return false;
    s_time_ok = true;
    ESP_LOGI(TAG, "clock set from the app");
    return true;
}

bool at_app_set_timezone(const char *zone)
{
    char posix[AT_TZ_MAX];
    if (!at_tz_resolve(zone, posix, sizeof(posix))) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_tz, posix, sizeof(s_tz));
    apply_tz(posix);
    xSemaphoreGive(s_lock);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_str(h, KEY_TZ, posix) == ESP_OK) nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "time zone %s", posix);
    return true;
}

static void load_tz(void)
{
    s_tz[0] = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    char buf[AT_TZ_MAX];
    size_t len = sizeof(buf);
    if (nvs_get_str(h, KEY_TZ, buf, &len) == ESP_OK && at_tz_valid_posix(buf)) {
        strlcpy(s_tz, buf, sizeof(s_tz));
        apply_tz(buf);
    }
    nvs_close(h);
}

/* ---- the list --------------------------------------------------------------------------------------- */

int at_app_timers_get(at_timer_t *out, int max)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_n < max ? s_n : max;
    memcpy(out, s_list, (size_t)n * sizeof(at_timer_t));
    xSemaphoreGive(s_lock);
    return n;
}

uint32_t at_app_timers_version(void)
{
    return s_version;
}

esp_err_t at_app_timers_set(const at_timer_t *list, int n, at_timer_problem_t *problem)
{
    float gap = at_app_band_min_gap();
    at_timer_t *cand = malloc(sizeof(at_timer_t) * AT_TIMERS_MAX);
    if (!cand) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    at_localtime_t now;
    local_now(&now);
    at_timer_problem_t p = at_timers_validate(list, n, s_list, s_n, &now, gap);
    if (problem) *problem = p;
    if (p.err != AT_TERR_NONE) {
        xSemaphoreGive(s_lock);
        free(cand);
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(cand, list, (size_t)n * sizeof(at_timer_t));
    int n2 = at_timers_prune_expired(cand, n, &now, s_runner.active ? s_runner.id : NULL);
    esp_err_t err = save_list_locked(cand, n2);
    if (err == ESP_OK) {
        memcpy(s_list, cand, (size_t)n2 * sizeof(at_timer_t));
        s_n = n2;
        s_version++;
    }
    xSemaphoreGive(s_lock);
    free(cand);
    return err == ESP_OK ? ESP_OK : ESP_FAIL;
}

void at_app_timer_status(at_timer_status_t *o)
{
    memset(o, 0, sizeof(*o));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    at_localtime_t now;
    local_now(&now);
    o->clock_ok = now.ok;
    o->time_set = s_time_ok;
    o->tz_set = s_tz[0] != 0;
    strlcpy(o->timezone, s_tz, sizeof(o->timezone));
    int idx;
    int64_t lm;
    if (now.ok && at_sched_next(s_list, s_n, &now, &idx, &lm)) {
        o->have_next = true;
        strlcpy(o->next_id, s_list[idx].id, sizeof(o->next_id));
        o->next_start_epoch = local_minute_to_epoch(lm);
    }
    o->active = s_runner.active;
    if (s_runner.active) strlcpy(o->active_id, s_runner.id, sizeof(o->active_id));
    strlcpy(o->note, s_runner.note, sizeof(o->note));
    xSemaphoreGive(s_lock);
}

/* ---- the task --------------------------------------------------------------------------------------- */

static bool op_preheat(void *ctx, int minutes) { return at_app_start_preheat(minutes); }
static bool op_begin(void *ctx, int low, int high) { return at_app_timer_thermostat_begin(low, high); }
static void op_end(void *ctx, bool disarm) { at_app_timer_thermostat_end(disarm); }
static const at_timer_ops_t s_ops = {op_preheat, op_begin, op_end, NULL};

static void timers_task(void *arg)
{
    char last_note[AT_TIMER_NOTE_LEN] = "";
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (s_n == 0 && !s_runner.active) continue;

        at_view_t v;
        at_app_view(&v);   /* takes the thermostat and core locks: not while holding s_lock */
        uint32_t now_ms = at_now_ms();

        xSemaphoreTake(s_lock, portMAX_DELAY);
        at_localtime_t now;
        local_now(&now);
        at_runner_tick(&s_runner, s_list, s_n, &now, now_ms, &v.snap, v.auto_enabled, &s_ops);
        int n2 = at_timers_prune_expired(s_list, s_n, &now, s_runner.active ? s_runner.id : NULL);
        if (n2 != s_n) {
            s_n = n2;
            s_version++;
            save_list_locked(s_list, s_n);
        }
        if (strcmp(last_note, s_runner.note)) {
            strlcpy(last_note, s_runner.note, sizeof(last_note));
            if (last_note[0]) ESP_LOGI(TAG, "%s", last_note);
        }
        xSemaphoreGive(s_lock);
    }
}

void at_timers_start(void)
{
    if (s_started) return;
    s_started = true;
    s_lock = xSemaphoreCreateMutex();
    at_runner_init(&s_runner);
    load_tz();
    load_list();
    /* A software restart keeps the system time on this chip; a power loss does not (it starts in 1970 and
     * the clock stays "not ok" until the network or the app sets it). */
    s_time_ok = time_plausible();
    ESP_LOGI(TAG, "%d timer(s), time zone '%s', clock %s", s_n, s_tz, s_time_ok ? "kept across the restart" : "not set");
    xTaskCreate(timers_task, "timers", 4096, NULL, 4, NULL);
}
