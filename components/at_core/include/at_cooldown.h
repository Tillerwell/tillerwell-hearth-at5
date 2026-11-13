/*
 * Heater cooldown (firmware 1.52): when the heater's own cooldown began, and how long it usually takes.
 *
 * The heater reports cooldown as status 0x04 (purge/cooldown) and 0x05 (final shutdown) before going idle. Hearth
 * notes the moment it first sees either, so the apps count down from when the heater really started cooling, not
 * from when an app happened to look. Every cooldown seen from its start to idle is kept (the last AT_CD_KEEP), and
 * the median is the typical length; until AT_CD_MIN_LEARN are known the default 5 minutes stands.
 *
 * Pure C over plain data, ticked by at_ctl with each status, so host_tests/ covers it. Times are uptime ms.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AT_CD_VERSION    1
#define AT_CD_KEEP       10        /* cooldowns kept, oldest first */
#define AT_CD_MIN_LEARN  2         /* cooldowns needed before the typical replaces the default */
#define AT_CD_DEFAULT_S  300u      /* until then: 5 minutes */
#define AT_CD_MIN_S      30u       /* shorter or longer than these: not a real cooldown, not kept */
#define AT_CD_MAX_S      1800u

/* Kept across a restart (one small NVS blob). */
typedef struct {
    uint8_t version;
    uint8_t n;
    uint16_t s[AT_CD_KEEP];   /* seconds, oldest first */
} at_cd_store_t;

typedef struct {
    at_cd_store_t st;
    bool dirty;               /* st changed: the caller saves it */
    bool seen_other;          /* a status other than cooldown has been seen since boot */
    bool on;                  /* the heater is cooling down now */
    bool start_known;         /* this cooldown was seen from its start (not already under way at boot) */
    uint32_t start_ms;
} at_cd_t;

void at_cd_init(at_cd_t *c);
/* Put a saved store back. False, and nothing changes, if it does not make sense. */
bool at_cd_restore(at_cd_t *c, const at_cd_store_t *st);
/* Each fresh heater status. */
void at_cd_tick(at_cd_t *c, uint8_t state_raw, uint32_t now_ms);
/* Cooling down now, and seconds since it began (false when not, or its start is not known). */
bool at_cd_elapsed(const at_cd_t *c, uint32_t now_ms, uint32_t *sec);
bool at_cd_active(const at_cd_t *c);
/* The typical cooldown in seconds: the median of those kept, or AT_CD_DEFAULT_S. *learned says which. */
uint32_t at_cd_typical(const at_cd_t *c, bool *learned);

#ifdef __cplusplus
}
#endif
