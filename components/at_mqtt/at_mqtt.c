#include "at_mqtt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "at_app.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"

static const char *TAG = "mqtt";

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static TaskHandle_t s_pub_task;
static char s_base[80];      /* hearth/<device> */
static char s_node[48];      /* object id prefix / device identifier */
static char s_uri[96];
static char s_user[40], s_pass[72];
static char s_prefix[24];

bool at_mqtt_connected(void) { return s_connected; }

/* ---- discovery table --------------------------------------------------------- */

typedef enum { E_SENSOR, E_BINARY, E_SWITCH, E_NUMBER, E_BUTTON } ekind_t;

typedef struct {
    ekind_t kind;
    const char *id;        /* object id, also the state field for sensors/binaries/switches/numbers */
    const char *name;
    const char *field;     /* JSON field in the state document (NULL for buttons) */
    const char *cmd;       /* command suffix for switch / number / button */
    const char *dc;        /* device_class */
    const char *unit;
    const char *sc;        /* state_class */
    const char *cat;       /* entity_category */
    const char *icon;
    float min, max, step;  /* number */
} ent_t;

static const ent_t ENTS[] = {
    {E_SENSOR, "state", "State", "state", NULL, "enum", NULL, NULL, NULL, "mdi:radiator", 0, 0, 0},
    {E_SENSOR, "state_code", "State code", "state_raw", NULL, NULL, NULL, NULL, "diagnostic", "mdi:radiator", 0, 0, 0},
    {E_SENSOR, "fault_code", "Fault code", "fault", NULL, NULL, NULL, NULL, "diagnostic", "mdi:alert-circle-outline", 0, 0, 0},
    {E_SENSOR, "fault_name", "Fault", "fault_name", NULL, NULL, NULL, NULL, NULL, "mdi:alert-circle-outline", 0, 0, 0},
    {E_BINARY, "fault_active", "Fault active", "fault_active", NULL, "problem", NULL, NULL, NULL, NULL, 0, 0, 0},
    {E_SENSOR, "last_fault_name", "Last fault", "last_fault_name", NULL, NULL, NULL, NULL, "diagnostic", "mdi:alert-circle-outline", 0, 0, 0},
    {E_SENSOR, "cabin_temp", "Temperature at controller", "cabin_temp", NULL, "temperature", "°C", "measurement", NULL, NULL, 0, 0, 0},
    {E_SENSOR, "coolant_temp", "Coolant temperature", "coolant_temp", NULL, "temperature", "°C", "measurement", NULL, NULL, 0, 0, 0},
    {E_SENSOR, "effective_temp", "Thermostat temperature", "effective_temp", NULL, "temperature", "°C", "measurement", NULL, NULL, 0, 0, 0},
    {E_SENSOR, "elapsed_min", "Elapsed run time", "elapsed_min", NULL, NULL, "min", NULL, "diagnostic", "mdi:timer-outline", 0, 0, 0},
    {E_BINARY, "burner_active", "Burner active", "burner_active", NULL, "heat", NULL, NULL, NULL, NULL, 0, 0, 0},
    {E_BINARY, "stale", "Telemetry stale", "stale", NULL, "problem", NULL, NULL, "diagnostic", NULL, 0, 0, 0},
    {E_BINARY, "external_temp_active", "Using external temperature", "external_temp_active", NULL, NULL, NULL, NULL, "diagnostic", NULL, 0, 0, 0},
    {E_SWITCH, "use_external_temp", "Use external temperature", "use_external_temp", "use_external_temp/set", NULL, NULL, NULL, "config", "mdi:thermometer-lines", 0, 0, 0},
    {E_SENSOR, "auto_note", "Auto thermostat note", "auto_note", NULL, NULL, NULL, NULL, "diagnostic", "mdi:message-text-outline", 0, 0, 0},
    {E_SWITCH, "prevent_freezing", "Prevent freezing", "prevent_freezing_enabled", "prevent_freezing/set", NULL, NULL, NULL, NULL, "mdi:snowflake-alert", 0, 0, 0},
    {E_NUMBER, "prevent_freezing_target", "Prevent freezing target", "prevent_freezing_target", "prevent_freezing_target/set", NULL, "°C", NULL, NULL, "mdi:thermometer-low", 0, 10, 0.5f},
    {E_SENSOR, "prevent_freezing_note", "Prevent freezing note", "prevent_freezing_note", NULL, NULL, NULL, NULL, "diagnostic", "mdi:message-text-outline", 0, 0, 0},
    {E_NUMBER, "preheat_minutes", "Preheat duration", "preheat_minutes", "preheat_minutes/set", NULL, "min", NULL, NULL, "mdi:timer-cog-outline", 1, 600, 1},
    {E_BINARY, "clock_ok", "Clock set (timers can run)", "clock_ok", NULL, NULL, NULL, NULL, "diagnostic", "mdi:clock-check-outline", 0, 0, 0},
    {E_SENSOR, "timer_note", "Last timer event", "timer_note", NULL, NULL, NULL, NULL, "diagnostic", "mdi:timer-cog-outline", 0, 0, 0},
    {E_BUTTON, "start_preheat", "Start preheat", NULL, "start_preheat", NULL, NULL, NULL, NULL, "mdi:fire", 0, 0, 0},
    {E_BUTTON, "start_thermostat", "Start thermostat (manual)", NULL, "start_thermostat", NULL, NULL, NULL, NULL, "mdi:thermostat", 0, 0, 0},
    {E_BUTTON, "stop", "Stop", NULL, "stop", NULL, NULL, NULL, NULL, "mdi:stop-circle-outline", 0, 0, 0},
    {E_BUTTON, "start_pump", "Start pump (ventilation only)", NULL, "start_pump", NULL, NULL, NULL, NULL, "mdi:fan", 0, 0, 0},
    {E_SWITCH, "bypass_mode", "Bypass (never send commands)", "bypass_mode", "bypass_mode/set", NULL, NULL, NULL, "config", "mdi:transit-connection-variant", 0, 0, 0},
    {E_SWITCH, "debug_mode", "Extended telemetry probing", "debug_mode", "debug_mode/set", NULL, NULL, NULL, "config", "mdi:bug-outline", 0, 0, 0},
    {E_BUTTON, "send_debug_handshake", "Send debug handshake now", NULL, "send_debug_handshake", NULL, NULL, NULL, "config", "mdi:handshake-outline", 0, 0, 0},
    {E_BINARY, "extended_active", "Extended telemetry active", "extended_active", NULL, NULL, NULL, NULL, "diagnostic", "mdi:radar", 0, 0, 0},
    {E_SENSOR, "ext_mode_name", "Mode of operation", "ext_mode_name", NULL, NULL, NULL, NULL, NULL, "mdi:state-machine", 0, 0, 0},
    {E_SENSOR, "ext_flame_temp", "Flame temperature", "ext_flame_temp_c", NULL, "temperature", "°C", "measurement", NULL, NULL, 0, 0, 0},
    {E_SENSOR, "ext_liquid_temp", "Liquid temperature (extended)", "ext_liquid_temp_c", NULL, "temperature", "°C", "measurement", NULL, NULL, 0, 0, 0},
    {E_SENSOR, "ext_overheat_temp", "Overheat sensor temperature", "ext_overheat_temp_c", NULL, "temperature", "°C", "measurement", NULL, NULL, 0, 0, 0},
    {E_SENSOR, "ext_board_temp", "Board temperature", "ext_board_temp_c", NULL, "temperature", "°C", "measurement", NULL, NULL, 0, 0, 0},
    {E_SENSOR, "ext_voltage", "Supply voltage", "ext_voltage", NULL, "voltage", "V", "measurement", NULL, NULL, 0, 0, 0},
    {E_SENSOR, "ext_fuel_pump_hz", "Fuel pump frequency", "ext_fuel_pump_hz", NULL, NULL, "Hz", "measurement", NULL, "mdi:gas-station-outline", 0, 0, 0},
    {E_SENSOR, "ext_output_pct", "Heater output", "ext_output_pct", NULL, NULL, "%", "measurement", NULL, "mdi:gauge", 0, 0, 0},
    {E_BINARY, "ext_glow_plug", "Glow plug", "ext_glow_plug", NULL, "heat", NULL, NULL, NULL, NULL, 0, 0, 0},
    {E_SENSOR, "ext_fault_name", "Fault (extended)", "ext_fault_name", NULL, NULL, NULL, NULL, "diagnostic", "mdi:alert-circle-outline", 0, 0, 0},
};

static void tpl_value(char *out, size_t n, const char *f)
{
    snprintf(out, n, "{{ value_json.%s if value_json.%s is not none else '' }}", f, f);
}

static void tpl_onoff(char *out, size_t n, const char *f)
{
    snprintf(out, n, "{{ 'ON' if value_json.%s else 'OFF' }}", f);
}

static void pub(const char *topic, const char *payload, int qos, bool retain)
{
    esp_mqtt_client_enqueue(s_client, topic, payload, 0, qos, retain, true);
}

static cJSON *device_obj(void)
{
    cJSON *d = cJSON_CreateObject();
    cJSON *ids = cJSON_AddArrayToObject(d, "identifiers");
    cJSON_AddItemToArray(ids, cJSON_CreateString(s_node));
    char nm[64];
    snprintf(nm, sizeof(nm), "Tillerwell Hearth (%s)", s_node + strlen("hearth_"));
    cJSON_AddStringToObject(d, "name", nm);
    cJSON_AddStringToObject(d, "manufacturer", "Tillerwell");
    cJSON_AddStringToObject(d, "model", "Hearth (bridge for Autoterm diesel heaters)");
    return d;
}

static void add_common(cJSON *o, const ent_t *e, const char *availability)
{
    char uid[96];
    snprintf(uid, sizeof(uid), "%s_%s", s_node, e->id);
    cJSON_AddStringToObject(o, "name", e->name);
    cJSON_AddStringToObject(o, "unique_id", uid);
    cJSON_AddStringToObject(o, "availability_topic", availability);
    cJSON_AddItemToObject(o, "device", device_obj());
    if (e->icon) cJSON_AddStringToObject(o, "icon", e->icon);
    if (e->cat) cJSON_AddStringToObject(o, "entity_category", e->cat);
}

static void publish_discovery(void)
{
    char avail[96], state[96], cmdt[128], topic[160], tpl[160];
    snprintf(avail, sizeof(avail), "%s/availability", s_base);
    snprintf(state, sizeof(state), "%s/state", s_base);

    for (size_t i = 0; i < sizeof(ENTS) / sizeof(ENTS[0]); i++) {
        const ent_t *e = &ENTS[i];
        const char *comp = e->kind == E_SENSOR ? "sensor" : e->kind == E_BINARY ? "binary_sensor" :
                           e->kind == E_SWITCH ? "switch" : e->kind == E_NUMBER ? "number" : "button";
        cJSON *o = cJSON_CreateObject();
        add_common(o, e, avail);
        if (e->kind != E_BUTTON) cJSON_AddStringToObject(o, "state_topic", state);
        if (e->cmd) {
            snprintf(cmdt, sizeof(cmdt), "%s/cmd/%s", s_base, e->cmd);
            cJSON_AddStringToObject(o, "command_topic", cmdt);
        }
        switch (e->kind) {
        case E_SENSOR:
            tpl_value(tpl, sizeof(tpl), e->field);
            cJSON_AddStringToObject(o, "value_template", tpl);
            if (e->dc) cJSON_AddStringToObject(o, "device_class", e->dc);
            if (e->unit) cJSON_AddStringToObject(o, "unit_of_measurement", e->unit);
            if (e->sc) cJSON_AddStringToObject(o, "state_class", e->sc);
            if (e->dc && !strcmp(e->dc, "enum")) {
                cJSON *opt = cJSON_AddArrayToObject(o, "options");
                static const char *const names[] = {"idle", "running", "late-run", "cooldown", "final-shutdown"};
                for (size_t k = 0; k < 5; k++) cJSON_AddItemToArray(opt, cJSON_CreateString(names[k]));
            }
            break;
        case E_BINARY:
        case E_SWITCH:
            tpl_onoff(tpl, sizeof(tpl), e->field);
            cJSON_AddStringToObject(o, "value_template", tpl);
            if (e->dc) cJSON_AddStringToObject(o, "device_class", e->dc);
            break;
        case E_NUMBER:
            tpl_value(tpl, sizeof(tpl), e->field);
            cJSON_AddStringToObject(o, "value_template", tpl);
            cJSON_AddNumberToObject(o, "min", e->min);
            cJSON_AddNumberToObject(o, "max", e->max);
            cJSON_AddNumberToObject(o, "step", e->step);
            cJSON_AddStringToObject(o, "mode", "box");
            if (e->unit) cJSON_AddStringToObject(o, "unit_of_measurement", e->unit);
            break;
        case E_BUTTON:
            break;
        }
        char *js = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
        if (js) {
            snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config", s_prefix, comp, s_node, e->id);
            pub(topic, js, 0, true);
            free(js);
        }
    }

    /* climate entity: the Auto thermostat */
    cJSON *o = cJSON_CreateObject();
    ent_t cl = {.id = "thermostat", .name = "Hearth thermostat"};
    add_common(o, &cl, avail);
    cJSON_AddItemToObject(o, "modes", cJSON_CreateStringArray((const char *[]){"off", "heat"}, 2));
    cJSON_AddStringToObject(o, "mode_state_topic", state);
    cJSON_AddStringToObject(o, "mode_state_template", "{{ 'heat' if value_json.auto_enabled else 'off' }}");
    snprintf(cmdt, sizeof(cmdt), "%s/cmd/auto_mode/set", s_base);
    cJSON_AddStringToObject(o, "mode_command_topic", cmdt);
    cJSON_AddStringToObject(o, "temperature_state_topic", state);
    cJSON_AddStringToObject(o, "temperature_state_template", "{{ value_json.auto_target }}");
    snprintf(cmdt, sizeof(cmdt), "%s/cmd/auto_target/set", s_base);
    cJSON_AddStringToObject(o, "temperature_command_topic", cmdt);
    cJSON_AddStringToObject(o, "current_temperature_topic", state);
    tpl_value(tpl, sizeof(tpl), "effective_temp");
    cJSON_AddStringToObject(o, "current_temperature_template", tpl);
    cJSON_AddStringToObject(o, "action_topic", state);
    cJSON_AddStringToObject(o, "action_template",
                            "{{ 'heating' if value_json.burner_active else ('idle' if value_json.auto_enabled else 'off') }}");
    cJSON_AddNumberToObject(o, "min_temp", 5);
    cJSON_AddNumberToObject(o, "max_temp", 35);
    cJSON_AddNumberToObject(o, "temp_step", 0.5);
    cJSON_AddStringToObject(o, "temperature_unit", "C");
    char *js = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (js) {
        snprintf(topic, sizeof(topic), "%s/climate/%s/thermostat/config", s_prefix, s_node);
        pub(topic, js, 0, true);
        free(js);
    }
}

/* ---- commands ------------------------------------------------------------------ */

static bool is_on(const char *p) { return !strcasecmp(p, "ON") || !strcasecmp(p, "true") || !strcmp(p, "1"); }

static void handle_cmd(const char *suffix, const char *payload)
{
    if (!strcmp(suffix, "start_preheat")) at_app_start_preheat(0);
    else if (!strcmp(suffix, "start_thermostat")) at_app_start_thermostat();
    else if (!strcmp(suffix, "stop")) at_app_stop();
    else if (!strcmp(suffix, "start_pump")) at_app_start_pump();
    else if (!strcmp(suffix, "send_debug_handshake")) at_app_send_pubr0();
    else if (!strcmp(suffix, "preheat_minutes/set")) at_app_set_preheat_minutes((int)atof(payload));
    else if (!strcmp(suffix, "auto_mode/set")) at_app_set_auto(!strcasecmp(payload, "heat"));
    else if (!strcmp(suffix, "auto_target/set")) at_app_set_auto_target(atof(payload));
    else if (!strcmp(suffix, "prevent_freezing/set")) at_app_set_freeze(is_on(payload));
    else if (!strcmp(suffix, "prevent_freezing_target/set")) at_app_set_freeze_target(atof(payload));
    else if (!strcmp(suffix, "debug_mode/set")) at_app_set_debug(is_on(payload), 0);
    else if (!strcmp(suffix, "bypass_mode/set")) at_app_set_bypass(is_on(payload));
    else if (!strcmp(suffix, "use_external_temp/set")) at_app_set_use_external_temp(is_on(payload));
    else if (!strcmp(suffix, "external_temp/set")) {
        char *end;
        double v = strtod(payload, &end);
        if (end != payload && v > -60 && v < 80) at_app_set_external_temp((float)v);
    } else {
        ESP_LOGW(TAG, "unknown command %s", suffix);
    }
}

static void publish_state(void)
{
    char *js = at_app_state_json();
    if (js) {
        char topic[96];
        snprintf(topic, sizeof(topic), "%s/state", s_base);
        pub(topic, js, 0, true);
        free(js);
    }
}

/* The timer list, same JSON as GET /api/timers, retained. Edit it by publishing the whole array to
 * <base>/cmd/timers/set; the outcome appears on <base>/timers/result. */
static uint32_t s_timers_version = UINT32_MAX;

static void publish_timers(void)
{
    char *js = at_app_timers_json();
    if (js) {
        char topic[96];
        snprintf(topic, sizeof(topic), "%s/timers", s_base);
        pub(topic, js, 0, true);
        free(js);
    }
    s_timers_version = at_app_timers_version();
}

static void publish_timers_result(int code, const char *err)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", code == 200);
    if (code != 200) cJSON_AddStringToObject(o, "error", err);
    char *js = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (js) {
        char topic[96];
        snprintf(topic, sizeof(topic), "%s/timers/result", s_base);
        pub(topic, js, 0, false);
        free(js);
    }
}

#define TIMERS_MAX_BODY 10240
static char *s_tbuf;          /* a timers/set message being reassembled from fragments */
static bool s_tcollect;

/* Returns true if the event belonged to the timers/set topic (and was handled). */
static bool handle_timers_data(esp_mqtt_event_handle_t ev)
{
    if (ev->current_data_offset == 0) {
        free(s_tbuf);
        s_tbuf = NULL;
        char want[100];
        snprintf(want, sizeof(want), "%s/cmd/timers/set", s_base);
        s_tcollect = (size_t)ev->topic_len == strlen(want) && !memcmp(ev->topic, want, (size_t)ev->topic_len);
        if (s_tcollect && ev->total_data_len > 0 && ev->total_data_len <= TIMERS_MAX_BODY) {
            s_tbuf = malloc((size_t)ev->total_data_len + 1);
        }
    }
    if (!s_tcollect) return false;
    if (s_tbuf) memcpy(s_tbuf + ev->current_data_offset, ev->data, (size_t)ev->data_len);
    if (ev->current_data_offset + ev->data_len >= ev->total_data_len) {
        char err[160] = "payload too large or out of memory";
        int code = 400;
        if (s_tbuf) {
            s_tbuf[ev->total_data_len] = 0;
            code = at_app_timers_put_json(s_tbuf, err, sizeof(err));
        }
        free(s_tbuf);
        s_tbuf = NULL;
        s_tcollect = false;
        publish_timers_result(code, err);
        publish_timers();
    }
    return true;
}

static void pub_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (!s_connected) continue;
        publish_state();
        if (at_app_timers_version() != s_timers_version) publish_timers();
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        s_connected = true;
        ESP_LOGI(TAG, "connected");
        char t[96];
        snprintf(t, sizeof(t), "%s/availability", s_base);
        pub(t, "online", 0, true);
        publish_discovery();
        snprintf(t, sizeof(t), "%s/cmd/#", s_base);
        esp_mqtt_client_subscribe(s_client, t, 0);
        publish_state();
        publish_timers();
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        break;
    case MQTT_EVENT_DATA: {
        if (handle_timers_data(ev)) break;
        char topic[128], payload[64];
        if (ev->topic_len <= 0 || (size_t)ev->topic_len >= sizeof(topic) || ev->data_len >= (int)sizeof(payload) ||
            ev->current_data_offset != 0 || ev->data_len != ev->total_data_len) {
            break;
        }
        memcpy(topic, ev->topic, ev->topic_len);
        topic[ev->topic_len] = 0;
        memcpy(payload, ev->data, ev->data_len);
        payload[ev->data_len] = 0;
        char pre[100];
        snprintf(pre, sizeof(pre), "%s/cmd/", s_base);
        if (!strncmp(topic, pre, strlen(pre))) handle_cmd(topic + strlen(pre), payload);
        break;
    }
    default:
        break;
    }
}

void at_mqtt_reload(void)
{
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        s_connected = false;
    }
    at_mqtt_start();
}

esp_err_t at_mqtt_start(void)
{
    if (s_client) {
        return ESP_OK;
    }
    at_settings_t cfg;
    at_settings_get(&cfg);
    if (!cfg.mqtt_enabled || !cfg.mqtt_host[0]) {
        return ESP_OK;
    }
    snprintf(s_base, sizeof(s_base), "hearth/%s", cfg.device_name);
    snprintf(s_node, sizeof(s_node), "hearth_%s", cfg.device_name);
    for (char *p = s_node; *p; p++) {
        if (*p == '-') *p = '_';
    }
    strlcpy(s_prefix, cfg.mqtt_discovery_prefix, sizeof(s_prefix));
    snprintf(s_uri, sizeof(s_uri), "mqtt://%s:%u", cfg.mqtt_host, cfg.mqtt_port);
    strlcpy(s_user, cfg.mqtt_user, sizeof(s_user));
    strlcpy(s_pass, cfg.mqtt_pass, sizeof(s_pass));
    static char lwt_topic[96], client_id[48];
    snprintf(lwt_topic, sizeof(lwt_topic), "%s/availability", s_base);
    snprintf(client_id, sizeof(client_id), "%s", s_node);

    esp_mqtt_client_config_t mc = {
        .broker.address.uri = s_uri,
        .credentials.client_id = client_id,
        .credentials.username = s_user[0] ? s_user : NULL,
        .credentials.authentication.password = s_pass[0] ? s_pass : NULL,
        .session.keepalive = 30,
        .session.last_will = {.topic = lwt_topic, .msg = "offline", .msg_len = 7, .qos = 0, .retain = true},
        .buffer.size = 2048,
        .buffer.out_size = 2048,
    };
    s_client = esp_mqtt_client_init(&mc);
    if (!s_client) {
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_event, NULL);
    if (!s_pub_task) {
        xTaskCreate(pub_task, "mqtt_pub", 4096, NULL, 4, &s_pub_task);
    }
    ESP_LOGI(TAG, "connecting to %s as %s", s_uri, s_node);
    return esp_mqtt_client_start(s_client);
}
