#pragma once
#include <stdbool.h>
#include "at_hrec.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The backup's "heater_record": {"version":"0a010d03","settings":"<hex>","settings_at":<epoch>,
 * "report":"<512 hex>","read_at":<epoch>,"seen":[10 epochs],"first_read":<bits>}. NULL when nothing was read. */
cJSON *at_hrec_to_json(const at_hrec_t *r);
/* Parses one; false if it holds nothing usable. */
bool at_hrec_from_json(const cJSON *o, at_hrec_t *r);

#ifdef __cplusplus
}
#endif
