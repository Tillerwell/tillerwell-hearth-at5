/*
 * Time zones for the timers. The box has no time zone database, so it works in POSIX TZ strings
 * ("GMT0BST,M3.5.0/1,M10.5.0"). A small table maps the IANA names a phone or a person would type
 * to those strings; anything else is accepted if it is itself a valid POSIX TZ string.
 * Pure C, host-tested.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AT_TZ_MAX 48   /* longest POSIX string kept, including the NUL */

/* True if s is a complete POSIX TZ string this firmware can use (standard name and offset, and if a
 * daylight name is given, both change-over rules). */
bool at_tz_valid_posix(const char *s);

/* Resolve an IANA name from the built-in table or a valid POSIX string into out. False if neither. */
bool at_tz_resolve(const char *in, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
