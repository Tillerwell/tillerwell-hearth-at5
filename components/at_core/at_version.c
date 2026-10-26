#include "at_version.h"

#include <ctype.h>
#include <stddef.h>

long at_roamos_version_number(const char *s)
{
    if (!s) return -1;
    if (!isdigit((unsigned char)s[0]) || s[1] != '.') return -1;
    long n = s[0] - '0';
    for (int i = 2; i < 6; i++) {
        if (!isdigit((unsigned char)s[i])) return -1;
        n = n * 10 + (s[i] - '0');
    }
    if (s[6] != '\0' || n == 0) return -1;
    return n;
}

at_update_check_t at_update_check(const char *running, const char *incoming, bool allow_older)
{
    if (allow_older) return AT_UPDATE_OK;
    long in = at_roamos_version_number(incoming);
    if (in < 0) return AT_UPDATE_NOT_ROAMOS;
    long cur = at_roamos_version_number(running);
    if (cur >= 0 && in < cur) return AT_UPDATE_OLDER;
    return AT_UPDATE_OK;
}
