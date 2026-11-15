/* The heater record in JSON (firmware 1.70, docs/API_SPEC.md §7.5 "heater_record"): the backup's copy and its
 * restore. Only cJSON here, so host_tests builds it (make -C host_tests hrec). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "at_hrec_json.h"

static void hex_out(char *dst, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) snprintf(dst + i * 2, 3, "%02x", p[i]);
    dst[n * 2] = 0;
}

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* exactly n bytes, or false */
static bool hex_in(const char *s, uint8_t *p, size_t n)
{
    if (!s || strlen(s) != n * 2) return false;
    for (size_t i = 0; i < n; i++) {
        int a = hexv(s[2 * i]), b = hexv(s[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        p[i] = (uint8_t)(a << 4 | b);
    }
    return true;
}

cJSON *at_hrec_to_json(const at_hrec_t *r)
{
    if (!r->have) return NULL;
    char *hx = malloc(AT_EEPROM_LEN * 2 + 1); /* off the caller's stack */
    cJSON *o = hx ? cJSON_CreateObject() : NULL;
    if (!o) {
        free(hx);
        return NULL;
    }
    if (r->have & AT_HREC_HAVE_VER) {
        hex_out(hx, r->heater_version, 4);
        cJSON_AddStringToObject(o, "version", hx);
    }
    if (r->have & AT_HREC_HAVE_SET) {
        hex_out(hx, r->settings, r->settings_len);
        cJSON_AddStringToObject(o, "settings", hx);
        cJSON_AddNumberToObject(o, "settings_at", r->settings_at);
    }
    if (r->have & AT_HREC_HAVE_REP) {
        hex_out(hx, r->eeprom, AT_EEPROM_LEN);
        cJSON_AddStringToObject(o, "report", hx);
        cJSON_AddNumberToObject(o, "read_at", r->read_at);
        cJSON *s = cJSON_AddArrayToObject(o, "seen");
        for (int k = 0; k < AT_FAULT_HISTORY_N; k++) cJSON_AddItemToArray(s, cJSON_CreateNumber(r->seen[k]));
        cJSON_AddNumberToObject(o, "first_read", r->first_read);
    }
    free(hx);
    return o;
}

static uint32_t u32(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) && v->valuedouble > 0 && v->valuedouble < 4294967296.0 ? (uint32_t)v->valuedouble : 0;
}

bool at_hrec_from_json(const cJSON *o, at_hrec_t *r)
{
    at_hrec_init(r);
    if (!cJSON_IsObject(o)) return false;
    if (hex_in(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "version")), r->heater_version, 4)) {
        r->have |= AT_HREC_HAVE_VER;
    }
    const char *st = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "settings"));
    size_t sl = st ? strlen(st) / 2 : 0;
    if (sl >= AT_SETTINGS_MIN && sl <= sizeof(r->settings) && hex_in(st, r->settings, sl)) {
        r->settings_len = (uint8_t)sl;
        r->settings_at = u32(o, "settings_at");
        r->have |= AT_HREC_HAVE_SET;
    }
    if (hex_in(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "report")), r->eeprom, AT_EEPROM_LEN)) {
        r->have |= AT_HREC_HAVE_REP;
        r->read_at = u32(o, "read_at");
        const cJSON *s = cJSON_GetObjectItemCaseSensitive(o, "seen");
        for (int k = 0; k < AT_FAULT_HISTORY_N && cJSON_IsArray(s); k++) {
            const cJSON *v = cJSON_GetArrayItem(s, k);
            r->seen[k] = cJSON_IsNumber(v) && v->valuedouble > 0 && v->valuedouble < 4294967296.0 ? (uint32_t)v->valuedouble : 0;
        }
        r->first_read = (uint16_t)(u32(o, "first_read") & ((1u << AT_FAULT_HISTORY_N) - 1));
        /* a column that is empty in the report has no date */
        for (int k = 0; k < AT_FAULT_HISTORY_N; k++) {
            if (!at_heater_fault_entry(r->eeprom, k)[0]) {
                r->seen[k] = 0;
                r->first_read &= (uint16_t)~(1u << k);
            }
        }
    }
    return r->have != 0;
}
