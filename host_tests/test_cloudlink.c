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

    // 1.75: start from the endpoint that last connected. Endpoint 1 remembered: 1, 1, 1, then 0, 0, 0, then 1 again.
    for (uint32_t f = 0; f < 12; f++)
        CHECK(at_cloudlink_endpoint_from(1, f, 2) == (int)((1 + f / 3) % 2), "from 1 of two, %u failures", f);
    CHECK(at_cloudlink_endpoint_from(2, 0, 3) == 2 && at_cloudlink_endpoint_from(2, 3, 3) == 0, "from the last of three wraps");
    CHECK(at_cloudlink_endpoint_from(7, 0, 2) == 0 && at_cloudlink_endpoint_from(-1, 0, 2) == 0, "a bad saved endpoint counts as 0");
    for (uint32_t f = 0; f < 13; f++)
        CHECK(at_cloudlink_endpoint_from(0, f, 3) == at_cloudlink_endpoint_for(f, 3), "from 0 is the old order, %u", f);

    // A host name that does not resolve moves to the next endpoint's first attempt.
    CHECK(at_cloudlink_failures_after_unresolved(0) == 3 && at_cloudlink_failures_after_unresolved(2) == 3, "first round: on to endpoint 1");
    CHECK(at_cloudlink_failures_after_unresolved(3) == 6 && at_cloudlink_failures_after_unresolved(5) == 6, "second round: on again");
    // The 18 Nov 2026 restart: nothing remembered, mqtt.tillerwell.com has no record. Before: three tries on 0 with
    // ~5 s between, ~36 s to online. Now the first failure lands straight on endpoint 1 after 1 s.
    uint32_t f0 = at_cloudlink_failures_after_unresolved(0);
    CHECK(at_cloudlink_endpoint_from(0, f0, 2) == 1, "the unresolved .com goes straight to .co.uk");
    CHECK(at_cloudlink_unresolved_delay_s(1, f0, 2) == AT_CLOUDLINK_UNRESOLVED_S, "after 1 s");
    // No DNS at all (the internet is down): one quick skip per endpoint, then the normal backoff, never 1 s for ever.
    CHECK(at_cloudlink_unresolved_delay_s(2, 6, 2) == at_cloudlink_retry_delay_s(6), "both names unresolved: back off");
    CHECK(at_cloudlink_unresolved_delay_s(9, 30, 2) == 300, "and the backoff still caps");
    CHECK(at_cloudlink_unresolved_delay_s(2, 3, 3) == AT_CLOUDLINK_UNRESOLVED_S, "three endpoints: two quick skips");
    CHECK(at_cloudlink_unresolved_delay_s(3, 9, 3) == at_cloudlink_retry_delay_s(9), "then the backoff");

    // Memory to dial: both the total and the largest block, at and either side of each limit.
    CHECK(at_cloudlink_heap_ok(44744, 14336), "healthy 1.0044 box after a restart dials");
    CHECK(!at_cloudlink_heap_ok(26908, 2496), "the 1.0044 box that answered 500 does not");
    CHECK(!at_cloudlink_heap_ok(38056, 7808), "plenty free but broken up does not");
    CHECK(at_cloudlink_heap_ok(AT_CLOUDLINK_DIAL_FREE_MIN, AT_CLOUDLINK_DIAL_LARGEST_MIN), "exactly at both limits");
    CHECK(!at_cloudlink_heap_ok(AT_CLOUDLINK_DIAL_FREE_MIN - 1, 60000), "one byte short of the total");
    CHECK(!at_cloudlink_heap_ok(90000, AT_CLOUDLINK_DIAL_LARGEST_MIN - 1), "one byte short of the block");
    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "OK", checks, failures);
    return failures != 0;
}
