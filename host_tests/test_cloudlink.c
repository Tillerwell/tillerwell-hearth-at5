// The cloud client's reconnect policy (docs/API_SPEC.md §9.5).   make -C host_tests cloudlink
#include <stdio.h>

#include "at_cloudlink_policy.h"

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    uint32_t want[] = { 5, 5, 5, 5, 5, 5, 10, 20, 40, 80 };
    for (uint32_t f = 1; f <= 10; f++)
        CHECK(at_cloudlink_retry_delay_s(f) == want[f - 1], "delay after %u failures: %u", f, at_cloudlink_retry_delay_s(f));
    CHECK(at_cloudlink_retry_delay_s(0) == 5, "zero failures");
    CHECK(at_cloudlink_retry_delay_s(12) == 300 && at_cloudlink_retry_delay_s(11) == 160, "reaches the cap after the quick tries");
    CHECK(at_cloudlink_retry_delay_s(1000000) == 300, "never past the cap, and no overflow");

    // N = 3 on each endpoint, in order, then round again.
    int e3[] = { 0, 0, 0, 1, 1, 1, 2, 2, 2, 0, 0, 0, 1 };
    for (uint32_t f = 0; f < 13; f++)
        CHECK(at_cloudlink_endpoint_for(f, 3) == e3[f], "three endpoints, %u failures: %d", f, at_cloudlink_endpoint_for(f, 3));
    for (uint32_t f = 0; f < 12; f++)
        CHECK(at_cloudlink_endpoint_for(f, 2) == (int)((f / 3) % 2), "two endpoints, %u failures", f);
    CHECK(at_cloudlink_endpoint_for(5, 0) == 0 && at_cloudlink_endpoint_for(5, 1) == 0, "degenerate lists");
    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "OK", checks, failures);
    return failures != 0;
}
