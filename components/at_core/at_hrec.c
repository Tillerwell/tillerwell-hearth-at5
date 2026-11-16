#include "at_hrec.h"
#include <string.h>

void at_hrec_init(at_hrec_t *r)
{
    memset(r, 0, sizeof(*r));
    r->version = AT_HREC_VERSION;
}

static bool empty_col(const uint8_t *e, int k) { return at_heater_fault_entry(e, k)[0] == 0; }

void at_hrec_live_add(at_hrec_lives_t *l, uint8_t code, uint32_t at, uint32_t ms)
{
    if (!code || !at) return;
    if (l->n < 0 || l->n > AT_HREC_LIVE_N) l->n = 0;
    if (l->n == AT_HREC_LIVE_N) {
        memmove(&l->f[0], &l->f[1], sizeof(l->f[0]) * (AT_HREC_LIVE_N - 1));
        l->n--;
    }
    l->f[l->n].code = code;
    l->f[l->n].at = at;
    l->f[l->n].ms = ms;
    l->n++;
}

static void live_keep(at_hrec_lives_t *l, const bool *drop)
{
    int w = 0;
    for (int i = 0; i < l->n; i++) {
        if (!drop[i]) l->f[w++] = l->f[i];
    }
    l->n = w;
}

void at_hrec_live_drop_before(at_hrec_lives_t *l, uint32_t ms)
{
    if (l->n < 0 || l->n > AT_HREC_LIVE_N) l->n = 0;
    bool drop[AT_HREC_LIVE_N] = {false};
    for (int i = 0; i < l->n; i++) drop[i] = (int32_t)(l->f[i].ms - ms) < 0;
    live_keep(l, drop);
}

bool at_hrec_upgrade(at_hrec_t *r)
{
    if (r->version == AT_HREC_VERSION) return true;
    if (r->version != 1) return false;
    r->exact = 0; /* padding in version 1 */
    r->version = AT_HREC_VERSION;
    return true;
}

bool at_hrec_take(at_hrec_t *r, const at_heater_info_t *hi, uint32_t epoch)
{
    return at_hrec_take_live(r, hi, epoch, NULL);
}

bool at_hrec_take_live(at_hrec_t *r, const at_heater_info_t *hi, uint32_t epoch, at_hrec_lives_t *lives)
{
    bool changed = false;
    if (hi->have_version && (!(r->have & AT_HREC_HAVE_VER) || memcmp(r->heater_version, hi->version, 4))) {
        memcpy(r->heater_version, hi->version, 4);
        r->have |= AT_HREC_HAVE_VER;
        changed = true;
    }
    if (hi->have_settings && hi->settings_len >= AT_SETTINGS_MIN && hi->settings_len <= sizeof(r->settings)) {
        if (!(r->have & AT_HREC_HAVE_SET) || r->settings_len != hi->settings_len ||
            memcmp(r->settings, hi->settings, hi->settings_len)) {
            memset(r->settings, 0, sizeof(r->settings));
            memcpy(r->settings, hi->settings, hi->settings_len);
            r->settings_len = hi->settings_len;
            r->have |= AT_HREC_HAVE_SET;
            changed = true;
        }
        if (epoch && r->settings_at != epoch) {
            r->settings_at = epoch;
            changed = true;
        }
    }
    if (hi->pages != (1u << AT_EEPROM_PAGES) - 1) return changed;

    bool first = !(r->have & AT_HREC_HAVE_REP);
    bool used[AT_HREC_LIVE_N] = {false};
    int nl = lives && lives->n > 0 ? (lives->n < AT_HREC_LIVE_N ? lives->n : AT_HREC_LIVE_N) : 0;
    /* oldest column first, so two new faults with the same code take the live times in the order they came */
    int newest = hi->eeprom[49] < AT_FAULT_HISTORY_N ? hi->eeprom[49] : 0;
    for (int i = 0; i < AT_FAULT_HISTORY_N; i++) {
        int k = (newest + 1 + i) % AT_FAULT_HISTORY_N;
        const uint8_t *now = at_heater_fault_entry(hi->eeprom, k);
        uint16_t bit = (uint16_t)(1u << k);
        if (empty_col(hi->eeprom, k)) {
            r->seen[k] = 0;
            r->first_read &= (uint16_t)~bit;
            r->exact &= (uint16_t)~bit;
        } else if (first) {
            r->seen[k] = epoch;
            r->first_read |= bit;
            r->exact &= (uint16_t)~bit;
        } else if (memcmp(now, at_heater_fault_entry(r->eeprom, k), AT_FAULT_ENTRY_LEN)) {
            int m = -1;
            for (int j = 0; j < nl && m < 0; j++) {
                if (!used[j] && lives->f[j].code == now[0] && lives->f[j].at && (!epoch || lives->f[j].at <= epoch)) m = j;
            }
            if (m >= 0) {
                used[m] = true;
                r->seen[k] = lives->f[m].at;
                r->exact |= bit;
            } else {
                r->seen[k] = epoch;
                r->exact &= (uint16_t)~bit;
            }
            r->first_read &= (uint16_t)~bit;
        } else if (!r->seen[k] && epoch && (r->first_read & bit)) {
            /* there on the first read but the clock was not set then: this read is the first date known */
            r->seen[k] = epoch;
            changed = true;
        }
    }
    if (nl) {
        lives->n = nl;
        live_keep(lives, used);
    }
    if (first || memcmp(r->eeprom, hi->eeprom, AT_EEPROM_LEN)) {
        memcpy(r->eeprom, hi->eeprom, AT_EEPROM_LEN);
        changed = true;
    }
    r->have |= AT_HREC_HAVE_REP;
    if (epoch && r->read_at != epoch) {
        r->read_at = epoch;
        changed = true;
    }
    return changed;
}

bool at_hrec_restore(at_hrec_t *r, const at_hrec_t *b)
{
    if (b->version != AT_HREC_VERSION) return false;
    if (r->version != AT_HREC_VERSION) return false;
    if (!(r->have & AT_HREC_HAVE_REP)) {
        if (!b->have) return false;
        /* the backup's report and dates, but a version or settings block this box has read itself stands */
        at_hrec_t n = *b;
        if (r->have & AT_HREC_HAVE_VER) memcpy(n.heater_version, r->heater_version, 4);
        if (r->have & AT_HREC_HAVE_SET) {
            memcpy(n.settings, r->settings, sizeof(n.settings));
            n.settings_len = r->settings_len;
            n.settings_at = r->settings_at;
        }
        n.have |= r->have;
        if (!memcmp(r, &n, sizeof(n))) return false;
        *r = n;
        return true;
    }
    bool changed = false;
    if (!(b->have & AT_HREC_HAVE_REP)) return false;
    for (int k = 0; k < AT_FAULT_HISTORY_N; k++) {
        if (empty_col(r->eeprom, k) || !b->seen[k]) continue;
        if (memcmp(at_heater_fault_entry(r->eeprom, k), at_heater_fault_entry(b->eeprom, k), AT_FAULT_ENTRY_LEN)) continue;
        uint16_t bit = (uint16_t)(1u << k);
        if (!r->seen[k] || b->seen[k] < r->seen[k]) {
            r->seen[k] = b->seen[k];
            r->first_read = (uint16_t)((r->first_read & ~bit) | (b->first_read & bit));
            r->exact = (uint16_t)((r->exact & ~bit) | (b->exact & bit));
            changed = true;
        }
    }
    return changed;
}

float at_hrec_preheat_temp(const at_hrec_t *r, float def)
{
    if (!(r->have & AT_HREC_HAVE_SET)) return def;
    at_heater_settings_t hs;
    if (!at_decode_heater_settings(r->settings, r->settings_len, &hs)) return def;
    if (hs.wait_temp < AT_SETTINGS_WAIT_TEMP_MIN || hs.wait_temp > AT_SETTINGS_WAIT_TEMP_MAX) return def;
    return (float)hs.wait_temp;
}
