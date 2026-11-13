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

`GET /api/timers` returns the list, `[]` when none. `PUT /api/timers` replaces the whole list; on success `{"ok":true}`, otherwise `400` and `{"ok":false,"error":"timer 2: too close to another timer: ... (timer 1)"}` with nothing changed (`500` if flash could not be written). Reading needs view, changing needs control (docs/API_SPEC.md §6).

From away, through the cloud, the list is changed one timer at a time with `POST /api/cmd` `{"cmd":"timer_set","timer":{…}}` (add, or replace by `id`) and `{"cmd":"timer_delete","id":"…"}`, each checked against the whole resulting list by the same rules. The box sends the list to the cloud (in its `backup`) whenever it changes. Details: docs/API_SPEC.md §9.9.

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
  "high_c": 22,
  "warm_by": false
}
```

`id` is kept as given. `mode` is `preheat` or `thermostat`. `start_minute` is minutes after local midnight (0 to 1439). `duration_min` is 30 to 480 (since firmware 1.47, Heater Care's minimum run; a timer saved earlier at 20 to 29 minutes still loads and runs, and is accepted again while its duration is unchanged). `days` are ISO weekdays (1 Monday to 7 Sunday), used when `date` is null and then must not be empty. `date` is a real local `yyyy-MM-dd` for a one-off, and then `days` is ignored. `low_c`/`high_c` are only read for `thermostat`: whole degrees, 0 to 30, low below high, at least 1 °C apart (since 1.47; Heater Care widens the working range at run time like the main range: the learned width, and at least 2 °C on the controller's whole-degree sensor). All numbers must be whole.

`warm_by` (firmware 1.56, thermostat only): start early so the room is at `high_c` at `start_minute`, not lighting at it ("Warm by" below). Refused on a preheat timer. **A timer that comes in without the key keeps the stored value for that id** (false for a new one): older apps, `timer_set` from away, MQTT, Home Assistant and the website do not know the field and would otherwise drop it. Only an explicit `false` clears it. Older firmware ignores the key.

Rules checked on every PUT, in this order per timer: id (letters, digits, `-`; unique), mode, day or date, duration, start, thermostat range, a one-off not already finished; then every pair of enabled timers for the 30 minute gap. At most 32 timers.

The gap rule treats each timer as `[start, start + duration + 30 min)`, and a `warm_by` timer as `[start − 120 min, start + duration + 30 min)`, so its early start cannot run into the timer before it. That wider rule applies to new or edited timers only: a pair saved earlier that clashes only because of it is left alone, and the warm-by timer's early start waits for the end of the other timer's gap. Repeating against repeating is compared on a circular week, so a run past midnight or past Sunday into Monday counts. One-off against one-off is compared on the calendar. A one-off against a repeating timer is compared on the one-off's weekday, including a repeating run that spills over from the day before. Disabled timers and finished one-offs are left out.

One deliberate softening of "a one-off may not be saved in the past": a finished one-off that is **unchanged from what is already stored** is accepted (and dropped), because the app may be saving a list it read just before the window ended. A new or edited one in the past is refused. Without a trusted clock nothing can be judged "past", so nothing is refused for that.

Finished one-offs are removed from the list a few seconds after their window ends (and one that was missed because the box was off is removed once the clock is ok).

## Status

`/api/status` (and the MQTT state topic) gain:

| Field | Meaning |
|---|---|
| `clock_ok` | Time and time zone are both trusted, so timers may fire |
| `timezone` | POSIX TZ string in use, `""` if not set |
| `next_timer` | `{"id","start_epoch","warm_by"}` of the next enabled start within a week, or `null`. For a warm-by timer also `expected_start_epoch` (inside the warming window the current prediction, recomputed each minute, or when it fired; before the window the start less the median lead of the last 5 warm-by runs, or less 60 min until there is one) and `estimate`: `"model"`, `"typical"` or `"fixed"` (docs/ADAPTIVE_THERMOSTAT.md §10.3). These change by the minute and are in the status only, never the backup |
| `active_timer_id` | The timer whose run is in progress, else `null` |
| `timer_note` | The last firing and what came of it, e.g. `06:30 skipped: heater already running`, `Warming for 07:00: started 06:12 (room 14.2°C, about 48 min)`, `Warm by 07:00: waiting (heater already running)`, `Warm by 07:00: started 05:00, capped at 2 h, may be late` |

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
- `thermostat`: arms Auto with `low_c..high_c` (widened to 1 °C if narrower), and at the end of the window turns Auto off (sending stop) and puts the previous band back. Since 1.47, if the window ends inside a run's 30 minute minimum run (from flame established), the stop waits until the minimum run is up; the status says `thermo.holding: "timer_ended"` and the apps show "Timer ended, finishing minimum run: N min left". A timer can therefore overrun its end by up to 30 minutes plus light-up. If the next thermostat timer starts by then, it takes the run over instead of skipping it as "heater already running". Nothing about a timer's Auto run is written to flash, so a reboot mid-run drops back to the user's own settings instead of leaving Auto armed.
- Manual override wins. Stopping a preheat by hand (or at the coolant limit) ends that timer's run and it does not restart in the same window; starting by hand makes any timer that comes due inside that run find the heater busy and skip. If the user switches Auto off during a thermostat window the run ends and nothing is touched.
- Existing safety limits are untouched: faults, the coolant limit and bypass all apply as for a manual start.
- **Warm by** (`warm_by`, 1.56): the timer has a **warming window** from 120 minutes before its start to the start. Once a minute in it the box works out how long the room needs (light-up, warm-up, the climb to `high_c` at the rate the space heats less what it is losing right now, a margin and a correction learned from past mornings: docs/ADAPTIVE_THERMOSTAT.md §10) and, once now + that lead reaches the start, arms Auto with the timer's range as a thermostat timer does. The lead is worked out on epoch seconds, so a clock change inside the window does not move it. It fires once per start: not again when the start minute passes. Its run ends with the window (start + duration), however early it began, and the 30 minute minimum run, the 20 minute rest and the top + 5 °C ceiling apply from the early start. A busy heater (running, a fault, bypass, another timer's run or its 30 minute gap, the user's own Auto, a start by hand) makes it **wait and try again the next minute**, not skip; only at the start time is the firing skipped for good. Frost protection's run is taken over. If the room is already inside the range when it fires, it makes one start there only when the climb from flame established is at least 30 minutes (Hearth's stop waits for 30 minutes of burning, so a shorter climb would overshoot); otherwise it leaves the room a little under the top. If the prediction never gets there, or the clock becomes trusted only at the start, it fires at the start time as normal. The clock becoming trusted inside the window is an early start, not a missed one: it may still fire. A restart after the early start leaves the heater running with Auto not armed, as for any thermostat timer.
- Daylight saving: if the clocks go forward and a start falls in the skipped hour it fires at the new time; if they go back, the repeated hour does not fire a timer twice (a timer that fired in the last 3 hours is not fired again).

## Other front ends

- **MQTT**: the list is published retained to `hearth/<name>/timers`; publish a whole array to `hearth/<name>/cmd/timers/set` to replace it; the outcome is on `hearth/<name>/timers/result` (`{"ok":true}` or `{"ok":false,"error":"..."}`). Home Assistant gets diagnostic entities "Clock set" and "Last timer event". The cloud broker is separate: see docs/API_SPEC.md §9.9.
- **Console**: `timers` shows the clock, the list, the next start and the last event; `timezone [name]` shows or sets the zone.
- **Web page**: no timers screen yet; the app is the interface.
- **Matter**: not exposed. The Thread-only image has no network time or API, so timers there stay inert (`clock_ok` false).

## Tests

`make -C host_tests` covers the rules (ported from `TimerRulesTests` plus midnight, week-end, thermostat range, count and date cases), firing with a fake clock (first poll, clock step, once per second, skipped runs, early end, thermostat arming and the hand-off, one-offs, back-to-back timers), the UK spring and autumn changes, warm by (`test_warm.inc`: the warming window, waiting for a busy heater, frost take-over, firing once, at the start when the prediction never gets there, the clock arriving inside the window, a clock change inside it, the wider gap rule, `warm_by` absent versus `false`), a run across midnight and Sunday into Monday, the next-timer calculation and the time zone table. The JSON layer was checked against the app's wire format with cJSON in a throwaway harness, not kept in the repo. Nothing has run on a real box yet.
