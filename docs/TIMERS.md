# Timers

The box owns a schedule of heater runs and fires them itself, so they happen when the phone is off, out of range or asleep. Open items are in `Hardware/TIMERS-TO-CONFIRM.md` in the Tillerwell planning folder. The iOS app's `TimerRules` (`HeaterTimer.swift`) is the reference for the rules; the box enforces them again because it must not trust the app.

Code map:

| Where | What |
|---|---|
| `components/at_core/at_timers.c` | Rules, "is anything due", the run a timer starts. Pure C, host-tested |
| `components/at_core/at_tz.c` | IANA name table and POSIX TZ string check. Pure C, host-tested |
| `components/at_app/at_app_timers.c` | Saved list (NVS), clock, time zone, SNTP, the one-second task |
| `components/at_app/at_json.c` | JSON in and out, status fields |
| `components/at_web/at_web.c`, `at_mqtt.c`, `main/console.c` | The front ends |

## API

`GET /api/timers` returns the list, `[]` when none. `PUT /api/timers` replaces the whole list; on success `{"ok":true}`, otherwise `400` and `{"ok":false,"error":"timer 2: too close to another timer: ... (timer 1)"}` with nothing changed (`500` if flash could not be written). Same optional web password as the rest of the API. The permission levels in the cloud doc (View to read, Control to change) do not exist in the firmware yet; they arrive with the signed-request work.

```json
{
  "id": "6F1C2E0A-1B7D-4F55-9C3A-0D2E5A6B7C8D",
  "enabled": true,
  "mode": "preheat",
  "start_minute": 390,
  "duration_min": 60,
  "days": [1, 2, 3, 4, 5],
  "date": null,
  "low_c": 19,
  "high_c": 22
}
```

`id` is kept as given. `mode` is `preheat` or `thermostat`. `start_minute` is minutes after local midnight (0 to 1439). `duration_min` is 20 to 480. `days` are ISO weekdays (1 Monday to 7 Sunday), used when `date` is null and then must not be empty. `date` is a real local `yyyy-MM-dd` for a one-off, and then `days` is ignored. `low_c`/`high_c` are only read for `thermostat`: whole degrees, 0 to 30, low below high, at least the hysteresis minimum apart (start + stop hysteresis, 3 °C by default). All numbers must be whole.

Rules checked on every PUT, in this order per timer: id (letters, digits, `-`; unique), mode, day or date, duration, start, thermostat range, a one-off not already finished; then every pair of enabled timers for the 30 minute gap. At most 32 timers.

The gap rule treats each timer as `[start, start + duration + 30 min)`. Repeating against repeating is compared on a circular week, so a run past midnight or past Sunday into Monday counts. One-off against one-off is compared on the calendar. A one-off against a repeating timer is compared on the one-off's weekday, including a repeating run that spills over from the day before. Disabled timers and finished one-offs are left out.

One deliberate softening of "a one-off may not be saved in the past": a finished one-off that is **unchanged from what is already stored** is accepted (and dropped), because the app may be saving a list it read just before the window ended. A new or edited one in the past is refused. Without a trusted clock nothing can be judged "past", so nothing is refused for that.

Finished one-offs are removed from the list a few seconds after their window ends (and one that was missed because the box was off is removed once the clock is ok).

## Status

`/api/status` (and the MQTT state topic) gain:

| Field | Meaning |
|---|---|
| `clock_ok` | Time and time zone are both trusted, so timers may fire |
| `timezone` | POSIX TZ string in use, `""` if not set |
| `next_timer` | `{"id","start_epoch"}` of the next enabled start within a week, or `null` |
| `active_timer_id` | The timer whose run is in progress, else `null` |
| `timer_note` | The last firing and what came of it, e.g. `06:30 skipped: heater already running` |

`/api/info` also reports `timezone`.

## Clock and time zone

- **Time** comes from SNTP (`pool.ntp.org`) once the box has an IP address, or from the app with `POST /api/cmd {"cmd":"set_time","epoch":<unix seconds>}`. The app's time is refused once network time has arrived this boot, so a phone with a wrong clock cannot override NTP. A boat with no internet therefore needs the app to set the time after every power loss.
- **Power loss** clears the clock: the hardware has no battery RTC. A software restart (OTA, crash, `reboot`) keeps it on this chip, and the box uses it. The clock after a power loss is deliberately **not** restored from a saved copy: it would be wrong by however long the power was off, and a heater must not start at a guessed hour. `clock_ok` stays `false` until real time arrives, and no timer fires meanwhile.
- **Time zone** is stored as a POSIX TZ string (the chip has no zone database). Set it with `POST /api/cmd {"cmd":"timezone","value":"Europe/London"}` or the console (`timezone Europe/London`). The value may be an IANA name from the built-in list of about 50 common zones (`components/at_core/at_tz.c`) or any complete POSIX string, e.g. `GMT0BST,M3.5.0/1,M10.5.0`. Anything else is refused (`{"ok":false}`). Until a zone is set, `clock_ok` is `false`.

## When a timer fires

Once per second the box compares the local clock with the schedule.

- A timer fires when its start minute has just passed. **Nothing fires on the first check after the clock becomes ok** (a missed firing is never run late), nor across a clock jump of more than 3 hours.
- It starts only if the heater is idle with fresh status, has no fault, bypass is off and no other timer run is going. Otherwise that firing is **skipped, not queued** (see `timer_note`). A thermostat timer is also skipped if Auto is already on: the user's own Auto wins.
- `preheat`: sends the same commands as `start_preheat` with `duration_min`. The heater ends the run itself; the coolant limit still stops it early.
- `thermostat`: arms Auto with `low_c..high_c` (widened if narrower than the hysteresis minimum), and at the end of the window turns Auto off (sending stop) and puts the previous band back. Nothing about a timer's Auto run is written to flash, so a reboot mid-run drops back to the user's own settings instead of leaving Auto armed.
- Manual override wins. Stopping a preheat by hand (or at the coolant limit) ends that timer's run and it does not restart in the same window; starting by hand makes any timer that comes due inside that run find the heater busy and skip. If the user switches Auto off during a thermostat window the run ends and nothing is touched.
- Existing safety limits are untouched: faults, the coolant limit and bypass all apply as for a manual start.
- Daylight saving: if the clocks go forward and a start falls in the skipped hour it fires at the new time; if they go back, the repeated hour does not fire a timer twice (a timer that fired in the last 3 hours is not fired again).

## Other front ends

- **MQTT**: the list is published retained to `hearth/<name>/timers`; publish a whole array to `hearth/<name>/cmd/timers/set` to replace it; the outcome is on `hearth/<name>/timers/result` (`{"ok":true}` or `{"ok":false,"error":"..."}`). Home Assistant gets diagnostic entities "Clock set" and "Last timer event". The cloud broker topics and the signed-command path are part of the cloud work, not done here.
- **Console**: `timers` shows the clock, the list, the next start and the last event; `timezone [name]` shows or sets the zone.
- **Web page**: no timers screen yet; the app is the interface.
- **Matter**: not exposed. The Thread-only image has no network time or API, so timers there stay inert (`clock_ok` false).

## Tests

`make -C host_tests` covers the rules (ported from `TimerRulesTests` plus midnight, week-end, thermostat range, count and date cases), firing with a fake clock (first poll, clock step, once per second, skipped runs, early end, thermostat arming and the hand-off, one-offs, back-to-back timers), the UK spring and autumn changes, a run across midnight and Sunday into Monday, the next-timer calculation and the time zone table. The JSON layer was checked against the app's wire format with cJSON in a throwaway harness, not kept in the repo. Nothing has run on a real box yet.
