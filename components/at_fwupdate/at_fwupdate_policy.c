#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "at_fwupdate.h"
#include "at_version.h"
#include "cJSON.h"

static const char *str_of(const cJSON *j, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

const char *at_fwupdate_parse(const char *cmd_json, const char *running, uint32_t max_size, at_fwupdate_req_t *out)
{
    cJSON *j = cmd_json ? cJSON_Parse(cmd_json) : NULL;
    if (!j) return "malformed";
    const char *reason = "malformed";
    const char *ver = str_of(j, "version"), *url = str_of(j, "url"), *sha = str_of(j, "sha256");
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(j, "size");
    if (!ver || !url || !sha || !cJSON_IsNumber(size)) goto done;
    if (strlen(ver) != 6 || at_roamos_version_number(ver) < 0) goto done;
    if (strlen(sha) != 64) goto done;
    for (int i = 0; i < 64; i++)
        if (!isdigit((unsigned char)sha[i]) && !(sha[i] >= 'a' && sha[i] <= 'f')) goto done;
    if (size->valuedouble < (double)AT_FWUPDATE_MIN_SIZE || size->valuedouble > 16777216.0) goto done;
    uint32_t n = (uint32_t)size->valuedouble;

    // Only our own release assets: .../releases/download/<tag>/hearth-<version>.bin, nothing after it.
    char tail[24];
    snprintf(tail, sizeof(tail), "/hearth-%s.bin", ver);
    size_t ul = strlen(url), pl = strlen(AT_FWUPDATE_URL_PREFIX), tl = strlen(tail);
    reason = "bad_url";
    if (ul >= AT_FWUPDATE_URL_MAX || ul < pl + 2 + tl || strncmp(url, AT_FWUPDATE_URL_PREFIX, pl) != 0) goto done;
    if (strcmp(url + ul - tl, tail) != 0) goto done;
    for (size_t i = pl; i < ul; i++)
        if (!isalnum((unsigned char)url[i]) && !strchr("./-_", url[i])) goto done;
    if (strstr(url + pl, "..") || strchr(url + pl, '?')) goto done;
    const char *slash = strchr(url + pl, '/');   // exactly one path segment (the tag) before the file name
    if (!slash || slash + tl != url + ul) goto done;

    reason = "older";
    if (at_update_check(running, ver, false) != AT_UPDATE_OK) goto done;
    reason = "too_big";
    if (n > max_size) goto done;

    if (out) {
        snprintf(out->version, sizeof(out->version), "%s", ver);
        snprintf(out->url, sizeof(out->url), "%s", url);
        snprintf(out->sha256, sizeof(out->sha256), "%s", sha);
        out->size = n;
    }
    reason = NULL;
done:
    cJSON_Delete(j);
    return reason;
}
