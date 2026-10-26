/*
 * RoamOS versions: one digit, a point and four digits ("0.1234"), counted per product (brand
 * guidelines, Naming). As a whole number, the first digit x 10000 + the four digits, so 0.1234 is
 * 1234 and 1.0002 is 10002; the build uses the same rule for Matter's software version
 * (roamos_version.cmake). Pure C, host-tested.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The whole number for a RoamOS version string, or -1 if s is not one (0.0000 included). */
long at_roamos_version_number(const char *s);

typedef enum {
    AT_UPDATE_OK,          /* same or newer, or the running version is not RoamOS so there is nothing to compare */
    AT_UPDATE_OLDER,       /* the image is an older RoamOS version */
    AT_UPDATE_NOT_ROAMOS,  /* the image's version is not in RoamOS form */
} at_update_check_t;

/* Whether an uploaded image may replace the running one. allow_older lets a rollback through
 * (either refusal); it is the user's explicit choice, never the default. */
at_update_check_t at_update_check(const char *running, const char *incoming, bool allow_older);

#ifdef __cplusplus
}
#endif
