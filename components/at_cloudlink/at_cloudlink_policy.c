#include "at_cloudlink_policy.h"

uint32_t at_cloudlink_retry_delay_s(uint32_t failures)
{
    uint32_t d = AT_CLOUDLINK_RETRY_FIRST_S;
    for (uint32_t i = AT_CLOUDLINK_QUICK_TRIES; i < failures && d < AT_CLOUDLINK_RETRY_MAX_S; i++) d *= 2;
    return d > AT_CLOUDLINK_RETRY_MAX_S ? AT_CLOUDLINK_RETRY_MAX_S : d;
}

int at_cloudlink_endpoint_for(uint32_t failures, int n)
{
    return at_cloudlink_endpoint_from(0, failures, n);
}

int at_cloudlink_endpoint_from(int first, uint32_t failures, int n)
{
    if (n <= 0) return 0;
    if (first < 0 || first >= n) first = 0;
    return (int)(((uint32_t)first + failures / AT_CLOUDLINK_ATTEMPTS_PER_ENDPOINT) % (uint32_t)n);
}

uint32_t at_cloudlink_failures_after_unresolved(uint32_t failures)
{
    return (failures / AT_CLOUDLINK_ATTEMPTS_PER_ENDPOINT + 1) * AT_CLOUDLINK_ATTEMPTS_PER_ENDPOINT;
}

uint32_t at_cloudlink_unresolved_delay_s(uint32_t in_row, uint32_t failures, int n)
{
    return n > 0 && in_row < (uint32_t)n ? AT_CLOUDLINK_UNRESOLVED_S : at_cloudlink_retry_delay_s(failures);
}

bool at_cloudlink_heap_ok(uint32_t free_bytes, uint32_t largest_block)
{
    return free_bytes >= AT_CLOUDLINK_DIAL_FREE_MIN && largest_block >= AT_CLOUDLINK_DIAL_LARGEST_MIN;
}
