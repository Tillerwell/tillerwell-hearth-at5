#include "at_tz.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

static const struct { const char *iana, *posix; } TABLE[] = {
    {"UTC", "UTC0"},
    {"Etc/UTC", "UTC0"},
    {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Dublin", "GMT0IST,M3.5.0/1,M10.5.0"},
    {"Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0"},
    {"Atlantic/Canary", "WET0WEST,M3.5.0/1,M10.5.0"},
    {"Atlantic/Madeira", "WET0WEST,M3.5.0/1,M10.5.0"},
    {"Atlantic/Azores", "<-01>1<+00>,M3.5.0/0,M10.5.0/1"},
    {"Atlantic/Reykjavik", "GMT0"},
    {"Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Madrid", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Rome", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Amsterdam", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Brussels", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Zurich", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Vienna", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Stockholm", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Oslo", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Copenhagen", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Prague", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Warsaw", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Malta", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Gibraltar", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Kyiv", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Bucharest", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Istanbul", "<+03>-3"},
    {"America/New_York", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Toronto", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Chicago", "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Denver", "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Phoenix", "MST7"},
    {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Vancouver", "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"America/Halifax", "AST4ADT,M3.2.0,M11.1.0"},
    {"Asia/Dubai", "<+04>-4"},
    {"Asia/Kolkata", "IST-5:30"},
    {"Asia/Singapore", "<+08>-8"},
    {"Asia/Hong_Kong", "HKT-8"},
    {"Asia/Tokyo", "JST-9"},
    {"Africa/Johannesburg", "SAST-2"},
    {"Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia/Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia/Brisbane", "AEST-10"},
    {"Australia/Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"Australia/Perth", "AWST-8"},
    {"Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
};

/* ---- POSIX TZ string checker ------------------------------------------------------------------------ */

static bool name(const char **p)
{
    const char *s = *p;
    if (*s == '<') {
        s++;
        const char *b = s;
        while (*s && *s != '>' && (isalnum((unsigned char)*s) || *s == '+' || *s == '-')) s++;
        if (*s != '>' || s == b) return false;
        *p = s + 1;
        return true;
    }
    const char *b = s;
    while (isalpha((unsigned char)*s)) s++;
    if (s - b < 3) return false;
    *p = s;
    return true;
}

static bool number(const char **p, int max_digits, int max_val)
{
    const char *s = *p;
    int v = 0, n = 0;
    while (isdigit((unsigned char)*s) && n < max_digits) {
        v = v * 10 + (*s - '0');
        s++;
        n++;
    }
    if (n == 0 || v > max_val) return false;
    *p = s;
    return true;
}

/* [+-]hh[:mm[:ss]] ; hours up to 167 as in POSIX rule times, 24 for offsets is enforced by the caller */
static bool hms(const char **p, bool signed_ok, int max_hours)
{
    const char *s = *p;
    if (signed_ok && (*s == '+' || *s == '-')) s++;
    if (!number(&s, 3, max_hours)) return false;
    if (*s == ':') {
        s++;
        if (!number(&s, 2, 59)) return false;
        if (*s == ':') {
            s++;
            if (!number(&s, 2, 59)) return false;
        }
    }
    *p = s;
    return true;
}

static bool rule(const char **p)
{
    const char *s = *p;
    if (*s == 'M') {
        s++;
        if (!number(&s, 2, 12) || *s++ != '.' || !number(&s, 1, 5) || *s++ != '.' || !number(&s, 1, 6)) return false;
    } else if (*s == 'J') {
        s++;
        if (!number(&s, 3, 365)) return false;
    } else if (!number(&s, 3, 365)) {
        return false;
    }
    if (*s == '/') {
        s++;
        if (!hms(&s, true, 167)) return false;
    }
    *p = s;
    return true;
}

bool at_tz_valid_posix(const char *str)
{
    if (!str || !str[0] || strlen(str) >= AT_TZ_MAX) return false;
    const char *s = str;
    if (!name(&s) || !hms(&s, true, 24)) return false;
    if (*s == 0) return true;                       /* no daylight saving */
    if (!name(&s)) return false;
    if (*s != ',' && *s != 0 && !hms(&s, true, 24)) return false;
    if (*s++ != ',' || !rule(&s) || *s++ != ',' || !rule(&s)) return false;
    return *s == 0;
}

bool at_tz_resolve(const char *in, char *out, size_t out_len)
{
    if (!in || !in[0]) return false;
    for (size_t i = 0; i < sizeof(TABLE) / sizeof(TABLE[0]); i++) {
        if (!strcasecmp(in, TABLE[i].iana)) {
            if (strlen(TABLE[i].posix) >= out_len) return false;
            strcpy(out, TABLE[i].posix);
            return true;
        }
    }
    if (!at_tz_valid_posix(in) || strlen(in) >= out_len) return false;
    strcpy(out, in);
    return true;
}
