/*
 * Heater Care's run log across a restart (firmware 1.55): the last AT_LEARN_RUNS runs behind GET /api/thermo/history
 * "runs" and the status's rise_rate. Until 1.54 the log was RAM only, so every restart and firmware update emptied
 * the "Why?" screen even though everything learned was kept.
 *
 * Kept apart from the at_learn store (its own small versioned NVS blob, "ctl_runs"), so nothing already learned is
 * touched by the update. `size` is sizeof(at_learn_run_t): a later change to the run's layout drops the saved log
 * rather than reading it wrongly. Saved when a run is added (at_runlog_changed against the last copy saved), once
 * per run.
 *
 * Pure C over plain data, so host_tests/ covers it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "at_learn.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AT_RUNLOG_VERSION 1

typedef struct {
    uint8_t version;
    uint8_t n;                          /* runs kept, oldest first */
    uint16_t size;                      /* sizeof(at_learn_run_t) when saved */
    at_learn_run_t r[AT_LEARN_RUNS];
} at_runlog_store_t;

/* The log as it is now, for saving. */
void at_runlog_save(const at_learn_t *L, at_runlog_store_t *out);
/* A run was added since `saved` was taken (or put back): the log needs saving. */
bool at_runlog_changed(const at_learn_t *L, const at_runlog_store_t *saved);
/* Put a saved log back. False, and nothing changes, if it does not make sense. */
bool at_runlog_restore(at_learn_t *L, const at_runlog_store_t *st);

#ifdef __cplusplus
}
#endif
