#include "at_timers.h"

#include <stdio.h>
#include <string.h>

#define WEEK_MIN (7 * 1440)

/* ---- calendar (Howard Hinnant's civil-date algorithms) ----------------------------------------------- */

int32_t at_days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int32_t)(era * 146097 + doe - 719468);
}

void at_civil_from_days(int32_t z, int *y, int *m, int *d)
{
    z += 719468;
    int era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = z - era * 146097;
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int yy = yoe + era * 400;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp + (mp < 10 ? 3 : -9);
    *y = yy + (*m <= 2);
}

int at_iso_weekday(int32_t days)
{
    return (int)(((days + 3) % 7 + 7) % 7) + 1; /* day 0 (1970-01-01) was a Thursday */
}

bool at_parse_date(const char *s, int32_t *days)
{
    if (!s || strlen(s) != 10 || s[4] != '-' || s[7] != '-') return false;
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) continue;
        if (s[i] < '0' || s[i] > '9') return false;
    }
    int y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
    int m = (s[5] - '0') * 10 + (s[6] - '0');
    int d = (s[8] - '0') * 10 + (s[9] - '0');
    if (y < 1970 || y > 2999 || m < 1 || m > 12 || d < 1) return false;
    int32_t n = at_days_from_civil(y, m, d);
    int y2, m2, d2;
    at_civil_from_days(n, &y2, &m2, &d2); /* rejects 31 February and friends */
    if (y2 != y || m2 != m || d2 != d) return false;
    *days = n;
    return true;
}

void at_format_date(int32_t days, char out[11])
{
    int y, m, d;
    at_civil_from_days(days, &y, &m, &d);
    y = y < 0 ? 0 : y % 10000;
    char *p = out;
    for (int div = 1000; div; div /= 10) *p++ = (char)('0' + y / div % 10);
    *p++ = '-';
    *p++ = (char)('0' + m / 10 % 10);
    *p++ = (char)('0' + m % 10);
    *p++ = '-';
    *p++ = (char)('0' + d / 10 % 10);
    *p++ = (char)('0' + d % 10);
    *p = 0;
}

/* ---- rules -------------------------------------------------------------------------------------------- */

const char *at_timer_err_text(at_timer_err_t e)
{
    switch (e) {
    case AT_TERR_NONE: return "ok";
    case AT_TERR_TOO_MANY: return "too many timers (32 at most)";
    case AT_TERR_BAD_ID: return "a timer needs an id";
    case AT_TERR_DUPLICATE_ID: return "two timers share an id";
    case AT_TERR_BAD_MODE: return "mode must be preheat or thermostat";
    case AT_TERR_BAD_START: return "start_minute must be 0 to 1439";
    case AT_TERR_BAD_DURATION: return "a timer runs for 20 minutes to 8 hours";
    case AT_TERR_NO_DAYS: return "pick at least one day (1 = Monday to 7 = Sunday)";
    case AT_TERR_BAD_DATE: return "date must be a real yyyy-MM-dd";
    case AT_TERR_BAD_RANGE: return "thermostat range must be 0 to 30 C, low below high, and at least the hysteresis apart";
    case AT_TERR_IN_PAST: return "that time has already passed";
    case AT_TERR_TOO_CLOSE: return "too close to another timer: leave 30 minutes between one ending and the next starting";
    }
    return "invalid";
}

bool at_timer_expired(const at_timer_t *t, const at_localtime_t *now)
{
    if (!t->one_off || !now->ok) return false;
    return (int64_t)t->date_day * 1440 + t->start_minute + t->duration_min <= at_local_minutes(now);
}

typedef struct { int64_t start; int length; } span_t;

static int64_t floor_mod(int64_t a, int64_t m) { return ((a % m) + m) % m; }

/* Whether two [start, start + length) spans overlap on the circular week. */
static bool overlap(span_t a, span_t b)
{
    int64_t d = floor_mod(b.start - a.start, WEEK_MIN);
    return d < a.length || WEEK_MIN - d < b.length;
}

static int rep_spans(const at_timer_t *t, span_t out[7])
{
    int n = 0;
    for (int wd = 1; wd <= 7; wd++) {
        if (t->days & (1u << (wd - 1))) {
            out[n].start = (wd - 1) * 1440 + t->start_minute;
            out[n].length = t->duration_min + AT_TIMER_GAP_MIN;
            n++;
        }
    }
    return n;
}

static bool clash_once_rep(const at_timer_t *once, const at_timer_t *rep)
{
    span_t mine = {(int64_t)(at_iso_weekday(once->date_day) - 1) * 1440 + once->start_minute,
                   once->duration_min + AT_TIMER_GAP_MIN};
    span_t r[7];
    int n = rep_spans(rep, r);
    for (int i = 0; i < n; i++) {
        if (overlap(mine, r[i])) return true;
    }
    return false;
}

static bool clash(const at_timer_t *a, const at_timer_t *b)
{
    if (a->one_off && b->one_off) {
        int64_t x = (int64_t)a->date_day * 1440 + a->start_minute, lx = a->duration_min + AT_TIMER_GAP_MIN;
        int64_t y = (int64_t)b->date_day * 1440 + b->start_minute, ly = b->duration_min + AT_TIMER_GAP_MIN;
        return x < y + ly && y < x + lx;
    }
    if (a->one_off) return clash_once_rep(a, b);
    if (b->one_off) return clash_once_rep(b, a);
    span_t sa[7], sb[7];
    int na = rep_spans(a, sa), nb = rep_spans(b, sb);
    for (int i = 0; i < na; i++) {
        for (int j = 0; j < nb; j++) {
            if (overlap(sa[i], sb[j])) return true;
        }
    }
    return false;
}

static bool same_timer(const at_timer_t *a, const at_timer_t *b)
{
    return !strcmp(a->id, b->id) && a->enabled == b->enabled && a->mode == b->mode && a->start_minute == b->start_minute &&
           a->duration_min == b->duration_min && a->days == b->days && a->one_off == b->one_off &&
           a->date_day == b->date_day && a->low_c == b->low_c && a->high_c == b->high_c;
}

static bool id_ok(const char *id)
{
    size_t n = 0;
    while (n < AT_TIMER_ID_LEN && id[n]) {
        char c = id[n];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-')) return false;
        n++;
    }
    return n > 0 && n < AT_TIMER_ID_LEN;
}

static at_timer_problem_t problem(at_timer_err_t e, int index, int other)
{
    at_timer_problem_t p = {e, index, other};
    return p;
}

at_timer_problem_t at_timers_validate(const at_timer_t *list, int n, const at_timer_t *existing, int n_existing,
                                      const at_localtime_t *now, float min_gap_c)
{
    if (n < 0 || n > AT_TIMERS_MAX) return problem(AT_TERR_TOO_MANY, -1, -1);
    bool ignore[AT_TIMERS_MAX] = {false};

    for (int i = 0; i < n; i++) {
        const at_timer_t *t = &list[i];
        if (!id_ok(t->id)) return problem(AT_TERR_BAD_ID, i, -1);
        for (int j = 0; j < i; j++) {
            if (!strcmp(list[j].id, t->id)) return problem(AT_TERR_DUPLICATE_ID, i, j);
        }
        if (t->mode != AT_TMODE_PREHEAT && t->mode != AT_TMODE_THERMOSTAT) return problem(AT_TERR_BAD_MODE, i, -1);
        if (t->one_off) {
            if (t->date_day < 0 || t->date_day > 365 * 1100) return problem(AT_TERR_BAD_DATE, i, -1);
        } else if ((t->days & 0x7F) == 0 || (t->days & 0x80)) {
            return problem(AT_TERR_NO_DAYS, i, -1);
        }
        if (t->duration_min < AT_TIMER_MIN_DUR || t->duration_min > AT_TIMER_MAX_DUR) {
            return problem(AT_TERR_BAD_DURATION, i, -1);
        }
        if (t->start_minute > 1439) return problem(AT_TERR_BAD_START, i, -1);
        if (t->mode == AT_TMODE_THERMOSTAT &&
            (t->low_c < 0 || t->high_c > 30 || t->low_c >= t->high_c || (float)(t->high_c - t->low_c) < min_gap_c)) {
            return problem(AT_TERR_BAD_RANGE, i, -1);
        }
        if (at_timer_expired(t, now)) {
            bool unchanged = false;
            for (int k = 0; k < n_existing && !unchanged; k++) unchanged = same_timer(&existing[k], t);
            if (!unchanged) return problem(AT_TERR_IN_PAST, i, -1);
            ignore[i] = true;
        }
    }
    for (int i = 0; i < n; i++) {
        if (!list[i].enabled || ignore[i]) continue;
        for (int j = 0; j < i; j++) {
            if (!list[j].enabled || ignore[j]) continue;
            if (clash(&list[i], &list[j])) return problem(AT_TERR_TOO_CLOSE, i, j);
        }
    }
    return problem(AT_TERR_NONE, -1, -1);
}

int at_timers_prune_expired(at_timer_t *list, int n, const at_localtime_t *now, const char *keep_id)
{
    int w = 0;
    for (int i = 0; i < n; i++) {
        if (at_timer_expired(&list[i], now) && !(keep_id && keep_id[0] && !strcmp(list[i].id, keep_id))) continue;
        if (w != i) list[w] = list[i];
        w++;
    }
    return w;
}

/* ---- firing ------------------------------------------------------------------------------------------- */

#define JUMP_LIMIT_MIN 180
#define REFIRE_GUARD_S (3 * 3600)

static bool starts_at(const at_timer_t *t, int32_t day, int minute)
{
    if (!t->enabled || t->start_minute != minute) return false;
    if (t->one_off) return t->date_day == day;
    return (t->days & (1u << (at_iso_weekday(day) - 1))) != 0;
}

static bool fired_recently(const at_sched_t *s, const char *id, int64_t epoch_s)
{
    for (int i = 0; i < AT_SCHED_RECENT; i++) {
        if (s->recent[i].id[0] && !strcmp(s->recent[i].id, id)) {
            int64_t d = epoch_s - s->recent[i].epoch_s;
            if (d < 0) d = -d;
            if (d < REFIRE_GUARD_S) return true;
        }
    }
    return false;
}

int at_sched_poll(at_sched_t *s, const at_timer_t *list, int n, const at_localtime_t *now)
{
    if (!now->ok) {
        s->have_prev = false;
        return -1;
    }
    int64_t lm = at_local_minutes(now);
    if (!s->have_prev) {
        s->have_prev = true;
        s->prev_lm = lm;
        return -1;
    }
    int64_t delta = lm - s->prev_lm;
    int64_t from = s->prev_lm;
    s->prev_lm = lm;
    if (delta <= 0 || delta > JUMP_LIMIT_MIN) return -1;

    int best = -1;
    for (int64_t m = from + 1; m <= lm; m++) {
        int32_t day = (int32_t)(m / 1440);
        int minute = (int)(m % 1440);
        for (int i = 0; i < n; i++) {
            if (starts_at(&list[i], day, minute)) best = i;
        }
    }
    if (best < 0 || fired_recently(s, list[best].id, now->epoch_s)) return -1;
    strlcpy(s->recent[s->recent_next].id, list[best].id, AT_TIMER_ID_LEN);
    s->recent[s->recent_next].epoch_s = now->epoch_s;
    s->recent_next = (s->recent_next + 1) % AT_SCHED_RECENT;
    return best;
}

bool at_sched_next(const at_timer_t *list, int n, const at_localtime_t *now, int *index, int64_t *start_lm)
{
    if (!now->ok) return false;
    int64_t lm = at_local_minutes(now), best = -1;
    int bi = -1;
    for (int i = 0; i < n; i++) {
        const at_timer_t *t = &list[i];
        if (!t->enabled) continue;
        int64_t cand = -1;
        if (t->one_off) {
            cand = (int64_t)t->date_day * 1440 + t->start_minute;
        } else {
            for (int d = 0; d <= 7 && cand < 0; d++) {
                int32_t day = now->day + d;
                if (!(t->days & (1u << (at_iso_weekday(day) - 1)))) continue;
                int64_t m = (int64_t)day * 1440 + t->start_minute;
                if (m > lm) cand = m;
            }
        }
        if (cand > lm && (best < 0 || cand < best)) {
            best = cand;
            bi = i;
        }
    }
    if (bi < 0) return false;
    *index = bi;
    *start_lm = best;
    return true;
}

/* ---- runs --------------------------------------------------------------------------------------------- */

void at_runner_init(at_runner_t *r)
{
    memset(r, 0, sizeof(*r));
}

static void note(at_runner_t *r, const at_timer_t *t, const char *what)
{
    snprintf(r->note, sizeof(r->note), "%02u:%02u %s", (unsigned)(t->start_minute / 60), (unsigned)(t->start_minute % 60), what);
}

void at_runner_tick(at_runner_t *r, const at_timer_t *list, int n, const at_localtime_t *now, uint32_t now_ms,
                    const at_snapshot_t *snap, bool auto_enabled, const at_timer_ops_t *ops)
{
    if (r->active) {
        uint32_t el = now_ms - r->start_ms;
        bool end = false, disarm = auto_enabled;
        const char *why = "finished";
        if (el >= r->length_ms) {
            end = true;
        } else if (r->mode == AT_TMODE_PREHEAT) {
            if (el > AT_RUN_START_GRACE_MS && snap->have_status && !snap->stale && snap->status.state_raw == 0x00) {
                end = true;
                why = "ended early: heater stopped";
            }
        } else if (!auto_enabled && el > 3000) {
            end = true;
            why = "ended early: Auto switched off";
        }
        if (end) {
            if (r->mode == AT_TMODE_THERMOSTAT) ops->thermostat_end(ops->ctx, disarm);
            r->active = false;
            r->note[0] = 0;
            snprintf(r->note, sizeof(r->note), "last run %s", why);
        }
    }

    int i = at_sched_poll(&r->sched, list, n, now);
    if (i < 0) return;
    const at_timer_t *t = &list[i];

    const char *skip = NULL;
    if (r->active) skip = "skipped: another timer is running";
    else if (!snap->have_status || snap->stale) skip = "skipped: no heater data";
    else if (snap->bypass) skip = "skipped: bypass is on";
    else if (snap->status.fault) skip = "skipped: heater fault";
    else if (snap->status.state_raw != 0x00) skip = "skipped: heater already running";
    else if (t->mode == AT_TMODE_THERMOSTAT && auto_enabled) skip = "skipped: Auto already on";
    if (skip) {
        note(r, t, skip);
        return;
    }

    bool ok = t->mode == AT_TMODE_PREHEAT ? ops->start_preheat(ops->ctx, t->duration_min)
                                          : ops->thermostat_begin(ops->ctx, t->low_c, t->high_c);
    if (!ok) {
        note(r, t, "skipped: command not sent");
        return;
    }
    r->active = true;
    strlcpy(r->id, t->id, sizeof(r->id));
    r->mode = t->mode;
    r->start_ms = now_ms;
    r->length_ms = (uint32_t)t->duration_min * 60000u;
    note(r, t, "started");
}
