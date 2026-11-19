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
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "timers";

#define NVS_NS        "autoterm"
#define KEY_TIMERS    "timers"
#define KEY_TZ        "tz"
#define KEY_UNTIL     "auto_until"   /* 1.74: the thermostat's run time, epoch seconds (int64); absent: none */
#define STORE_VERSION 3         /* 2 (1.56): warm_by, in what was padding in version 1 (same size); 3 (1.77): high_d,
                                 * the last padding byte */
#define MIN_EPOCH     1704067200LL   /* 2024-01-01: anything earlier is a clock that was never set */
#define MAX_EPOCH     4102444800LL   /* 2100-01-01 */
#define NET_TIME_MAGIC 0x4e544d31u   /* "NTM1": the clock running now came from the network (SNTP) */

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
static volatile bool s_sntp_synced;  /* ... from the network, this boot or carried over a software restart */
/* Set when SNTP sets the clock; a software restart leaves RTC memory and the system time alone, so after an update
 * or a Reboot the clock is still network time and the cloud link need not wait for a new NTP answer (1.75). A power
 * loss starts the clock again at 1970 and this word as garbage; the reset reason check below rules both out. */
static RTC_NOINIT_ATTR uint32_t s_net_time_mark;
static bool s_sntp_started;
static bool s_started;
static int64_t s_until;              /* 1.74: Auto goes off at this wall-clock time (0: no run time) */

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
            if (hdr.version >= 1 && hdr.version <= STORE_VERSION && hdr.item_size == sizeof(at_timer_t) &&
                hdr.count <= AT_TIMERS_MAX && size == sizeof(hdr) + hdr.count * sizeof(at_timer_t)) {
                at_timer_t tmp[AT_TIMERS_MAX];
                memcpy(tmp, buf + sizeof(hdr), hdr.count * sizeof(at_timer_t));
                /* version 1 had padding where warm_by now is: never trust what was in it */
                if (hdr.version == 1) {
                    for (uint32_t k = 0; k < hdr.count; k++) tmp[k].warm_by = 0;
                }
                /* versions 1 and 2 had padding where high_d now is: whole degrees, as they were */
                if (hdr.version < 3) {
                    for (uint32_t k = 0; k < hdr.count; k++) tmp[k].high_d = 0;
                }
                at_localtime_t noclock = {0};   /* nothing is "in the past" without a clock */
                at_timer_problem_t p = at_timers_validate(tmp, (int)hdr.count, tmp, (int)hdr.count, &noclock, 0);
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

static void save_until(int64_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t err = v ? nvs_set_i64(h, KEY_UNTIL, v) : nvs_erase_key(h, KEY_UNTIL);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "saving the thermostat run time failed: %s", esp_err_to_name(err));
}

static void load_until(void)
{
    s_until = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    int64_t v = 0;
    if (nvs_get_i64(h, KEY_UNTIL, &v) == ESP_OK && v > 0) s_until = v;
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

bool at_app_local_time(int32_t *day, int *minute)
{
    at_localtime_t now;
    local_now(&now);
    if (now.ok) {
        *day = now.day;
        *minute = now.minute;
    }
    return now.ok;
}

bool at_app_local_day(int32_t *day)
{
    at_localtime_t now;
    local_now(&now);
    if (now.ok) *day = now.day;
    return now.ok;
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
    s_net_time_mark = NET_TIME_MAGIC;
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

bool at_app_clock_synced(void) { return s_sntp_synced; }

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
    /* a sender that did not send warm_by keeps the stored value for that id */
    memcpy(cand, list, (size_t)n * sizeof(at_timer_t));
    at_timers_resolve_warm_by(cand, n, s_list, s_n);
    /* and one that did not send "high" (1.77) keeps the stored tenths while high_c is unchanged */
    at_timers_resolve_tenths(cand, n, s_list, s_n);
    at_timer_problem_t p = at_timers_validate(cand, n, s_list, s_n, &now, gap);
    if (problem) *problem = p;
    if (p.err != AT_TERR_NONE) {
        xSemaphoreGive(s_lock);
        free(cand);
        return ESP_ERR_INVALID_ARG;
    }
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
        o->next_earliest_epoch = o->next_start_epoch;
        const at_timer_t *t = &s_list[idx];
        if (t->mode == AT_TMODE_THERMOSTAT && t->warm_by == 1) {
            o->next_warm_by = true;
            o->next_earliest_epoch = o->next_start_epoch - AT_TIMER_WARM_MIN * 60;
            if (s_runner.warm_on && !strcmp(s_runner.warm_id, t->id) &&
                s_runner.warm_start_epoch == o->next_start_epoch && s_runner.warm_expected_epoch > 0) {
                /* inside the warming window: the live prediction, or when it fired */
                o->next_expected_epoch = s_runner.warm_expected_epoch;
                o->next_estimate = s_runner.warm_est;
            } else {
                /* before the window: the typical lead of the last warm-by runs, no forecast of the room */
                uint32_t lead;
                if (!at_app_warm_typical_lead(&lead)) lead = AT_WARM_FIXED_LEAD_MIN;
                o->next_expected_epoch = o->next_start_epoch - (int64_t)lead * 60;
                o->next_estimate = at_app_warm_estimate(at_timer_high(t));
            }
        }
    }
    o->active = s_runner.active;
    if (s_runner.active) strlcpy(o->active_id, s_runner.id, sizeof(o->active_id));
    if (s_runner.active && s_time_ok) {
        o->run_ends_epoch = (int64_t)time(NULL) + (at_runner_left_ms(&s_runner, at_now_ms()) + 999u) / 1000u;
    } else if (!s_runner.active && s_until && s_time_ok) {
        o->run_ends_epoch = s_until;
    }
    strlcpy(o->note, s_runner.note, sizeof(o->note));
    xSemaphoreGive(s_lock);
}

/* ---- the task --------------------------------------------------------------------------------------- */

static bool op_preheat(void *ctx, int minutes) { return at_app_start_preheat(minutes); }
static bool op_begin(void *ctx, int low, float high) { return at_app_timer_thermostat_begin(low, high); }
static void op_end(void *ctx, bool disarm) { at_app_timer_thermostat_end(disarm); }
static bool op_take_over(void *ctx) { return at_app_timer_can_take_over(); }
static bool op_warm_plan(void *ctx, const at_timer_t *t, int64_t start_epoch, at_warm_plan_t *out)
{
    return at_app_warm_plan(t, start_epoch, out);
}
static bool op_warm_begin(void *ctx, const at_timer_t *t, int64_t start_epoch, const at_warm_plan_t *plan, uint8_t hint,
                          float *room)
{
    return at_app_warm_begin(t, start_epoch, plan, hint, room);
}
static bool op_frost_run(void *ctx) { return at_app_timer_frost_run(); }
static int64_t op_local_to_epoch(void *ctx, int64_t lm) { return local_minute_to_epoch(lm); }
static const at_timer_ops_t s_ops = {op_preheat, op_begin, op_end, NULL, op_take_over,
                                     op_warm_plan, op_warm_begin, op_frost_run, op_local_to_epoch};

static void timers_task(void *arg)
{
    char last_note[AT_TIMER_NOTE_LEN] = "";
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (s_n == 0 && !s_runner.active && !s_until) continue;

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
        /* 1.74: the thermostat's run time. Auto switched off by anyone (or a timer's run, which has its own end)
         * drops it; once the time is reached Auto goes off. Kept through a restart; one that ran out while the box
         * was off ends Auto as soon as the clock is back. */
        bool until_clear = false, until_due = false;
        if (s_until) {
            if (!v.auto_enabled || s_runner.active) {
                until_clear = true;
            } else if (s_time_ok && time(NULL) >= s_until) {
                until_clear = until_due = true;
            }
            if (until_clear) s_until = 0;
        }
        /* 1.78: a timer's run has its own end, so it drops turn off once warm as it drops the run time */
        const bool once_clear = v.auto_once && s_runner.active;
        xSemaphoreGive(s_lock);
        if (until_clear) save_until(0);
        if (until_due) at_app_auto_until_end();
        if (once_clear) at_app_auto_once_set(false);
    }
}

/* ---- run end (1.74) --------------------------------------------------------------------------------- */

void at_app_timers_preheat_left(uint32_t left_ms)
{
    if (!s_started) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_runner.active && s_runner.mode == AT_TMODE_PREHEAT) at_runner_set_left(&s_runner, at_now_ms(), left_ms);
    xSemaphoreGive(s_lock);
}

bool at_app_auto_until_set(int minutes)
{
    if (!s_started || minutes < 0 || minutes > AT_RUN_END_MAX_MIN) return false;
    at_view_t v;
    at_app_view(&v);   /* takes the thermostat and core locks: not while holding s_lock */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = true;
    if (s_runner.active) {
        ok = false;                    /* a timer's run: its end is moved with run_end */
    } else if (minutes == 0) {
        s_until = 0;
    } else if (!v.auto_enabled || !s_time_ok) {
        ok = false;
    } else {
        s_until = (int64_t)time(NULL) + (int64_t)minutes * 60;
    }
    int64_t saved = s_until;
    xSemaphoreGive(s_lock);
    if (ok) {
        save_until(saved);
        if (saved) ESP_LOGI(TAG, "thermostat run time: Auto off in %d min", minutes);
        else ESP_LOGI(TAG, "thermostat run time removed");
    } else {
        ESP_LOGW(TAG, "thermostat run time refused (%s)", s_runner.active ? "a timer is running"
                                                         : !v.auto_enabled ? "Auto is off" : "clock not set");
    }
    return ok;
}

bool at_app_run_end(int minutes)
{
    if (!s_started || minutes < 0 || minutes > AT_RUN_END_MAX_MIN) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool active = s_runner.active;
    const uint8_t mode = s_runner.mode;
    if (active && mode == AT_TMODE_THERMOSTAT && minutes > 0) {
        if (minutes > AT_TIMER_MAX_DUR) minutes = AT_TIMER_MAX_DUR;
        at_runner_set_left(&s_runner, at_now_ms(), (uint32_t)minutes * 60000u);
        ESP_LOGI(TAG, "timer run now ends in %d min", minutes);
    }
    xSemaphoreGive(s_lock);
    if (active) {
        if (minutes == 0) return false;   /* a timer's run always has an end */
        if (mode == AT_TMODE_PREHEAT) return at_app_preheat_left(minutes > AT_TIMER_MAX_DUR ? AT_TIMER_MAX_DUR : minutes);
        return true;
    }
    at_view_t v;
    at_app_view(&v);
    if (v.auto_enabled) return at_app_auto_until_set(minutes);
    if (v.snap.run_mode == AT_RUN_PREHEAT && v.snap.have_status && v.snap.status.state_raw != 0x00 && minutes > 0) return at_app_preheat_left(minutes);
    return false;
}

void at_timers_start(void)
{
    if (s_started) return;
    s_started = true;
    s_lock = xSemaphoreCreateMutex();
    at_runner_init(&s_runner);
    load_tz();
    load_list();
    load_until();
    /* A software restart keeps the system time on this chip; a power loss does not (it starts in 1970 and
     * the clock stays "not ok" until the network or the app sets it). */
    s_time_ok = time_plausible();
    /* Network time kept across a software restart (an update, Reboot, a crash) counts as network time. A clock the
     * app set never does: it was not network time before the restart either, so the mark is not there. */
    const esp_reset_reason_t rr = esp_reset_reason();
    if (s_time_ok && s_net_time_mark == NET_TIME_MAGIC && (rr == ESP_RST_SW || rr == ESP_RST_PANIC)) s_sntp_synced = true;
    else s_net_time_mark = 0;
    ESP_LOGI(TAG, "%d timer(s), time zone '%s', clock %s", s_n, s_tz,
             s_sntp_synced ? "network time kept across the restart" : s_time_ok ? "kept across the restart" : "not set");
    xTaskCreate(timers_task, "timers", 4096, NULL, 4, NULL);
}
