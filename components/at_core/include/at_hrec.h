#pragma once
/* The heater's own record, kept by the box (firmware 1.70, docs/API_SPEC.md §9.8): the last complete read of the
 * heater's settings block, version and 256-byte operation report, saved so the apps show it straight away after a
 * restart, and the date the box first saw each fault in the report's history (the heater keeps no dates). It goes
 * into the backup ("heater_record") and comes back on a replacement box. Pure logic: no NVS or JSON here. */
#include <stdbool.h>
#include <stdint.h>
#include "at_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AT_HREC_VERSION   1
#define AT_HREC_HAVE_VER  0x01
#define AT_HREC_HAVE_SET  0x02
#define AT_HREC_HAVE_REP  0x04

typedef struct {
    uint8_t version;                    /* AT_HREC_VERSION */
    uint8_t have;                       /* AT_HREC_HAVE_* */
    uint8_t heater_version[4];
    uint8_t settings[12];
    uint8_t settings_len;
    uint8_t pad;
    uint16_t first_read;                /* bit k: fault column k was already in the report on the box's first read */
    uint32_t read_at;                   /* epoch of the last complete report read, 0 if the clock was not set */
    uint32_t settings_at;               /* epoch the settings block was last read */
    uint32_t seen[AT_FAULT_HISTORY_N];  /* epoch the box first saw fault column k as it is now, 0 unknown or empty */
    uint8_t eeprom[AT_EEPROM_LEN];
} at_hrec_t;

void at_hrec_init(at_hrec_t *r);

/* Takes what a read brought in: the version and settings if the heater sent them, the report only when all four
 * pages arrived since read_ms. A fault column whose 17 bytes are new or changed gets epoch as its date; on the very
 * first report every fault in it gets epoch and its first_read bit ("on or before"). Returns true if anything
 * changed (save it). */
bool at_hrec_take(at_hrec_t *r, const at_heater_info_t *hi, uint32_t epoch);

/* Puts back a record from a backup (PUT /api/backup). A box with no report of its own takes the whole record.
 * Otherwise, for every fault column whose bytes are the same in both, the earlier known date wins, with its
 * first_read bit; the box's own reads stand. Restoring the same record twice changes nothing. Returns true if
 * anything changed. */
bool at_hrec_restore(at_hrec_t *r, const at_hrec_t *b);

/* The coolant temperature a preheat stops at: the heater's own wait temperature from its settings block, or def
 * (the box's saved preheat_temp) until the heater has been read. */
float at_hrec_preheat_temp(const at_hrec_t *r, float def);

#ifdef __cplusplus
}
#endif
