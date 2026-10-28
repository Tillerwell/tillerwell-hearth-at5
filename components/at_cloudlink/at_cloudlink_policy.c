#include "at_cloudlink_policy.h"

uint32_t at_cloudlink_retry_delay_s(uint32_t failures)
{
    uint32_t d = AT_CLOUDLINK_RETRY_FIRST_S;
    for (uint32_t i = 1; i < failures && d < AT_CLOUDLINK_RETRY_MAX_S; i++) d *= 2;
    return d > AT_CLOUDLINK_RETRY_MAX_S ? AT_CLOUDLINK_RETRY_MAX_S : d;
}

int at_cloudlink_endpoint_for(uint32_t failures, int n)
{
    return n > 0 ? (int)((failures / AT_CLOUDLINK_ATTEMPTS_PER_ENDPOINT) % (uint32_t)n) : 0;
}
