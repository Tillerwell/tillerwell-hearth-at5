/* The status document's worst case against its print buffer (firmware 1.72).
 *
 * 1.70 added the heater record to every status, the document outgrew its 6 KB buffer (STATE_BUF_MAX) and
 * /api/status answered 500 for good, so every app lost the box. This builds the real document from at_json.c with
 * every input at its worst: every flag on, every number many digits long, every text field at its full length, all
 * ten heater faults with the longest fault and mode names. It fails when that comes within MARGIN of the buffer.
 *
 * The fakes fill each struct with the byte 0x01 (every bool true, every integer 16843009) and then give every
 * pointer and char array a real value. A field added later that is a pointer or a string and is not filled here
 * reads garbage and AddressSanitizer stops the test: that is deliberate, so the test cannot quietly go stale.
 *
 *   make -C host_tests status
 */
#include "../components/at_app/at_json.c"

#include <assert.h>

#define MARGIN 3072 /* room left for fields added later, before a release must look at the buffer again */
#define BIG_EPOCH 4000000000u

static char s_longest_fault[64], s_longest_mode[64];
static int s_longest_fault_code, s_longest_state, s_longest_sub;

static void fill(char *s, size_t n)
{
    memset(s, 'W', n - 1);
    s[n - 1] = 0;
}

static void find_longest_names(void)
{
    for (int c = 1; c < 256; c++) {
        const char *n = at_fault_name((uint8_t)c);
        if (n && strlen(n) > strlen(s_longest_fault)) {
            snprintf(s_longest_fault, sizeof(s_longest_fault), "%s", n);
            s_longest_fault_code = c;
        }
    }
    for (int st = 0; st < 16; st++) {
        for (int sub = 0; sub < 16; sub++) {
            const char *n = at_mode_name((uint8_t)st, (uint8_t)sub);
            if (n && strlen(n) > strlen(s_longest_mode)) {
                snprintf(s_longest_mode, sizeof(s_longest_mode), "%s", n);
                s_longest_state = st;
                s_longest_sub = sub;
            }
        }
    }
}

/* ---- fakes for the box's getters ------------------------------------------------------------------------------ */

const esp_app_desc_t *esp_app_get_description(void)
{
    static esp_app_desc_t d;
    fill(d.version, sizeof(d.version));
    fill(d.idf_ver, sizeof(d.idf_ver));
    return &d;
}

int64_t esp_timer_get_time(void)
{
    static int64_t t;
    return t += 2000000; /* every call is past STATE_FRESH_US, so each request builds afresh */
}

void at_app_view(at_view_t *v)
{
    memset(v, 0x01, sizeof(*v));
    v->snap.state_name = s_longest_mode;
    v->snap.fault_name = s_longest_fault;
    v->snap.last_fault_name = s_longest_fault;
    v->snap.ext.mode_name = s_longest_mode;
    v->snap.status.state_raw = (uint8_t)s_longest_state;
    v->snap.status.substate = (uint8_t)s_longest_sub;
    v->snap.status.fault = (uint8_t)s_longest_fault_code;
    fill(v->snap.last_cmd, sizeof(v->snap.last_cmd));
    v->snap.run_mode = AT_RUN_PREHEAT; /* the run mode that adds preheat_total_min */
    v->thermo.reason = "controller_sensor";
    fill(v->auto_note, sizeof(v->auto_note));
    fill(v->freeze_note, sizeof(v->freeze_note));
    v->clock_ok = true;
    v->epoch = BIG_EPOCH;
}

void at_app_heater_info(at_heater_info_t *out, uint32_t *now_ms)
{
    memset(out, 0x01, sizeof(*out));
    *now_ms = 0x7fffffff;
}

void at_app_hrec_copy(at_hrec_t *r)
{
    static const uint8_t blk[] = {0x58, 0x00, 0x55, 0x01, 0x28, 0x00, 0x00, 0x00, 0x00}; /* the cabin heater's */
    memset(r, 0x01, sizeof(*r));
    r->version = AT_HREC_VERSION;
    r->have = 0xff;
    memset(r->heater_version, 0xff, sizeof(r->heater_version));
    memset(r->settings, 0, sizeof(r->settings));
    memcpy(r->settings, blk, sizeof(blk));
    r->settings_len = sizeof(blk);
    r->first_read = 0x3ff;
    r->read_at = r->settings_at = 1;
    for (int k = 0; k < AT_FAULT_HISTORY_N; k++) r->seen[k] = BIG_EPOCH;
    memset(r->eeprom, 0xff, sizeof(r->eeprom));
    r->eeprom[0] = 0x55;
    r->eeprom[1] = 0xaa;
    r->eeprom[49] = AT_FAULT_HISTORY_N - 1;
    for (int k = 0; k < AT_FAULT_HISTORY_N; k++) {
        uint8_t *f = (uint8_t *)at_heater_fault_entry(r->eeprom, k);
        f[0] = (uint8_t)s_longest_fault_code;
        f[2] = (uint8_t)s_longest_state;
        f[3] = (uint8_t)s_longest_sub;
    }
}

bool at_app_hrec_retrying(void) { return true; }

void at_app_timer_status(at_timer_status_t *t)
{
    memset(t, 0x01, sizeof(*t));
    fill(t->timezone, sizeof(t->timezone));
    fill(t->next_id, sizeof(t->next_id));
    fill(t->active_id, sizeof(t->active_id));
    fill(t->note, sizeof(t->note));
    t->next_start_epoch = t->next_expected_epoch = t->next_earliest_epoch = BIG_EPOCH;
}

void at_app_fuel_view(at_fuel_view_t *f) { memset(f, 0x01, sizeof(*f)); }
void at_app_battery_view(at_battery_view_t *b) { memset(b, 0x01, sizeof(*b)); }
bool at_settings_auto_update(void) { return true; }
bool at_settings_mqtt(void) { return true; }
void at_settings_display_name(char *out, size_t len) { fill(out, len); }
int at_app_pump_minutes(void) { return 1440; }
int at_app_heater_rx_mv(void) { return 3300; }
void at_app_rx_probe(int high_pct[2], int edges[2])
{
    high_pct[0] = high_pct[1] = 100;
    edges[0] = edges[1] = 99999;
}

/* the rest of at_json.c (backup, fuel, timers) links against these but the status does not use them */
cJSON *at_app_hrec_backup(void) { return NULL; }
cJSON *at_app_thermo_care_backup(void) { return NULL; }
cJSON *at_app_thermo_learn_backup(void) { return NULL; }
char *at_app_fuel_cloud_json(void) { return NULL; }
bool at_app_fuel_detail(int32_t day, at_fuel_detail_day_t *d, at_fuel_day_t *l) { return false; }
int at_app_fuel_detail_days(int32_t *out, int max) { return 0; }
int at_app_fuel_log_copy(at_fuel_day_t *out, int max) { return 0; }
int at_app_timers_get(at_timer_t *out, int max) { return 0; }
esp_err_t at_app_timers_set(const at_timer_t *list, int n, at_timer_problem_t *p) { return ESP_OK; }

/* ---- the test ------------------------------------------------------------------------------------------------- */

int main(void)
{
    find_longest_names();
    at_app_state_json_init();
    char *js = at_app_state_json();
    assert(js && "the worst-case status did not build or did not fit its buffer at all");

    cJSON *o = cJSON_Parse(js);
    assert(o);
    const cJSON *rp = cJSON_GetObjectItem(cJSON_GetObjectItem(o, "heater_info"), "report");
    assert(rp && "heater_info.report missing: the fakes no longer reach it");
    const int faults = cJSON_GetArraySize(cJSON_GetObjectItem(rp, "faults"));
    const bool omitted = cJSON_IsTrue(cJSON_GetObjectItem(rp, "faults_omitted"));
    const size_t len = strlen(js);
    const size_t room = STATE_BUF_MAX - 5;
    printf("status worst case: %zu bytes of %zu (margin wanted %d, left %ld), %d faults%s\n", len, room, MARGIN,
           (long)room - (long)len, faults, omitted ? ", fault list OMITTED" : "");

    int fails = 0;
    if (omitted || faults != AT_FAULT_HISTORY_N) {
        printf("FAIL: the full status did not fit, so the box would drop the fault list\n");
        fails++;
    }
    if (len + MARGIN > room) {
        printf("FAIL: under %d bytes left in the status buffer: raise STATE_BUF_MAX in at_json.c (it is static RAM, "
               "check the heap) or move something out of the status\n", MARGIN);
        fails++;
    }
    cJSON_Delete(o);
    free(js);
    printf(fails ? "status size: FAILED\n" : "status size: ok\n");
    return fails ? 1 : 0;
}
