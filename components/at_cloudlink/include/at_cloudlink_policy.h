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

// Seconds to wait after the given number of failures in a row (>= 1): 5 for the first six, then 10, 20 ... capped at 300.
uint32_t at_cloudlink_retry_delay_s(uint32_t failures);
// Which of n endpoints to dial after the given number of failures in a row: N on each, in order, then round again.
int at_cloudlink_endpoint_for(uint32_t failures, int n);
// Whether there is memory to dial. Below it a handshake fails part-way, and each failed attempt leaves the heap more
// broken up than before (1.0044: eleven in a row, the largest block down to 2.5 KB and /api/status answering 500).
bool at_cloudlink_heap_ok(uint32_t free_bytes, uint32_t largest_block);

#ifdef __cplusplus
}
#endif
