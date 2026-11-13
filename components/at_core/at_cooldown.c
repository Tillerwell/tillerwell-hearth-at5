#include "at_cooldown.h"

#include <string.h>

void at_cd_init(at_cd_t *c)
{
    memset(c, 0, sizeof(*c));
    c->st.version = AT_CD_VERSION;
}

bool at_cd_restore(at_cd_t *c, const at_cd_store_t *st)
{
    if (st->version != AT_CD_VERSION || st->n > AT_CD_KEEP) return false;
    for (int i = 0; i < st->n; i++) {
        if (st->s[i] < AT_CD_MIN_S || st->s[i] > AT_CD_MAX_S) return false;
    }
    c->st = *st;
    c->dirty = false;
    return true;
}

static bool cooling(uint8_t st)
{
    return st == 0x04 || st == 0x05;
}

static void keep(at_cd_t *c, uint32_t sec)
{
    if (sec < AT_CD_MIN_S || sec > AT_CD_MAX_S) return;
    if (c->st.n == AT_CD_KEEP) {
        memmove(&c->st.s[0], &c->st.s[1], (AT_CD_KEEP - 1) * sizeof(c->st.s[0]));
        c->st.n--;
    }
    c->st.s[c->st.n++] = (uint16_t)sec;
    c->dirty = true;
}

void at_cd_tick(at_cd_t *c, uint8_t state_raw, uint32_t now_ms)
{
    if (cooling(state_raw)) {
        if (!c->on) {
            c->on = true;
            c->start_ms = now_ms;
            /* found already cooling at boot: when it began is not known */
            c->start_known = c->seen_other;
        }
        return;
    }
    c->seen_other = true;
    if (!c->on) return;
    c->on = false;
    /* idle (or locked/waiting) after it: a whole cooldown. Lit again (0x02/0x03): it was cut short, not kept. */
    if (c->start_known && (state_raw == 0x00 || state_raw == 0x01)) keep(c, (now_ms - c->start_ms) / 1000u);
}

bool at_cd_active(const at_cd_t *c)
{
    return c->on;
}

bool at_cd_elapsed(const at_cd_t *c, uint32_t now_ms, uint32_t *sec)
{
    if (!c->on || !c->start_known) return false;
    *sec = (now_ms - c->start_ms) / 1000u;
    return true;
}

uint32_t at_cd_typical(const at_cd_t *c, bool *learned)
{
    if (c->st.n < AT_CD_MIN_LEARN) {
        if (learned) *learned = false;
        return AT_CD_DEFAULT_S;
    }
    uint16_t v[AT_CD_KEEP];
    int n = c->st.n;
    memcpy(v, c->st.s, n * sizeof(v[0]));
    for (int i = 1; i < n; i++) {
        uint16_t x = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > x) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = x;
    }
    if (learned) *learned = true;
    return n % 2 ? v[n / 2] : (uint32_t)(v[n / 2 - 1] + v[n / 2] + 1) / 2u;
}
