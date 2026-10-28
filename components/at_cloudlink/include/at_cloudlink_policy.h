// When and where the cloud client reconnects (docs/API_SPEC.md §9.5). Pure arithmetic, host-tested.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AT_CLOUDLINK_ATTEMPTS_PER_ENDPOINT 3     // N: failed attempts on one endpoint before the next is tried
#define AT_CLOUDLINK_RETRY_FIRST_S 5
#define AT_CLOUDLINK_RETRY_MAX_S 300
#define AT_CLOUDLINK_STABLE_S 60                 // a connection that lasted this long counts as a success

// Seconds to wait after the given number of failures in a row (>= 1): 5, 10, 20 ... capped at 300.
uint32_t at_cloudlink_retry_delay_s(uint32_t failures);
// Which of n endpoints to dial after the given number of failures in a row: N on each, in order, then round again.
int at_cloudlink_endpoint_for(uint32_t failures, int n);

#ifdef __cplusplus
}
#endif
