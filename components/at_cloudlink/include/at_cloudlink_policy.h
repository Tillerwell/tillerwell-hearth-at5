// When and where the cloud client reconnects (docs/API_SPEC.md §9.5). Pure arithmetic, host-tested.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AT_CLOUDLINK_ATTEMPTS_PER_ENDPOINT 3     // N: failed attempts on one endpoint before the next is tried
#define AT_CLOUDLINK_RETRY_FIRST_S 5
#define AT_CLOUDLINK_RETRY_MAX_S 300
#define AT_CLOUDLINK_QUICK_TRIES 6               // the first attempts after a restart all wait RETRY_FIRST_S: the link is usually up in seconds
#define AT_CLOUDLINK_STABLE_S 60                 // a connection that lasted this long counts as a success
#define AT_CLOUDLINK_DIAL_FREE_MIN 30000         // heap a TLS session needs: about 20 KB of buffers, and room left after
#define AT_CLOUDLINK_DIAL_LARGEST_MIN 12288      // largest block: the handshake's biggest single allocation, with margin
#define AT_CLOUDLINK_UNRESOLVED_S 1              // wait after a host name that does not resolve: no server was reached, so try the next at once

// Seconds to wait after the given number of failures in a row (>= 1): 5 for the first six, then 10, 20 ... capped at 300.
uint32_t at_cloudlink_retry_delay_s(uint32_t failures);
// Which of n endpoints to dial after the given number of failures in a row: N on each, in order, then round again.
int at_cloudlink_endpoint_for(uint32_t failures, int n);
// The same, starting from endpoint `first` (the one that last connected, kept across restarts, 1.75) instead of the
// top of the list, so a restart dials the endpoint known to work first. Out-of-range `first` counts as 0.
int at_cloudlink_endpoint_from(int first, uint32_t failures, int n);
// The failure count after an attempt whose host name did not resolve (1.75). DNS has no record, so dialling the same
// name twice more is pointless: the count jumps to the first attempt on the next endpoint.
uint32_t at_cloudlink_failures_after_unresolved(uint32_t failures);
// Seconds to wait after an unresolved host name, the `in_row`-th in a row (>= 1). Each endpoint gets one quick
// skip (AT_CLOUDLINK_UNRESOLVED_S); once every endpoint failed to resolve in a row (no DNS at all, the internet is
// down) the normal backoff for `failures` applies, so a box with no DNS never dials every second.
uint32_t at_cloudlink_unresolved_delay_s(uint32_t in_row, uint32_t failures, int n);
// Whether there is memory to dial. Below it a handshake fails part-way, and each failed attempt leaves the heap more
// broken up than before (1.0044: eleven in a row, the largest block down to 2.5 KB and /api/status answering 500).
bool at_cloudlink_heap_ok(uint32_t free_bytes, uint32_t largest_block);

#ifdef __cplusplus
}
#endif
