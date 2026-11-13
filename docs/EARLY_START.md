# Early start inside the range: plan

Status: **built, firmware 1.60 (RoamOS 1.0060, 2026-11-13).** See §11 for where the build differs from the plan below. Decisions agreed with Chris on 2026-11-13. This adds to Heater Care ([ADAPTIVE_THERMOSTAT.md](ADAPTIVE_THERMOSTAT.md), which is built) and changes one of its rules: today every thermostat run starts at or below the bottom of the working range. After this, a run may start higher, between the bottom and the top, when Hearth is sure the run will still last its 30 minute minimum.

## 1. Why

With a set temperature of 19°C the user's range is 18–19. Today Hearth waits for the room to fall to 18 (or lower, when it has learned that the space heats fast), then heats to 19. The room swings across the whole degree.

The run log on the cabin box (pulled 2026-11-13, firmware 1.59) shows the room heats far more slowly than that range needs:

| Run | Start → peak | Burn | Warm-up before the room rises | Rate |
|---|---|---|---|---|
| 11 Nov 19:56 | 18.9 → 20.1 | 2 h 17 m | 49 min | 1.5°C/h (0.0255°C/min) |
| 12 Nov 17:41 | 14.8 → 18.4 (switched off) | 5 h 47 m | 16 min | 0.4°C/h |
| 13 Nov 13:12 | 14.6 → 19.0 | 7 h 16 m | 15 min | 0.28°C/h |

Even the fastest run would take about 70 minutes to climb half a degree (warm-up plus climb). So the 30 minute minimum run is never what stops an earlier start in this cabin. Starting at 18.7 instead of 18 keeps the room steadier at the same heater wear per run. The cost is more starts, which the busy rule (§2, decision 2) limits.

Caveats: only three useful runs, none counted for learning yet (`learned_width` 0), all after a long cold off period. Runs in normal cycling, with the walls warm, will climb faster. The design copes with that because the start point comes from the live rate, not from these numbers.

## 2. Decisions (agreed 2026-11-13)

| # | Topic | Decision |
|---|---|---|
| 1 | Narrowest band | **0.3°C.** With 19 set, the earliest start is 18.7. |
| 2 | Too many starts | **Widen when busy.** If Auto starts the heater too often (first figure: 3 starts in 2 hours), the band widens back toward the working range, and narrows again once the heater has been calm. No hard cap on starts. |
| 3 | Before anything is learned | **Halfway: start at top − 0.5°C** (18.5 for 19), never below the working bottom. |
| 4 | Controller's whole-degree sensor | **No change.** Early starts only on the external sensor. On the controller's sensor the start stays at the working bottom, as today. |
| 5 | Which rate | **Live: the learned heating rate less the room's current cooling**, worked out at each decision, as warm by does (`at_warm_predict`, model estimate). |

## 3. The rule

Terms: `lo`, `hi` = the working range (`at_ctl_working_range`, unchanged). `stop_at` = `hi` less the coast lead (`at_ctl_stop_at`, unchanged). `busy_w` = the extra width the busy rule has added (§5), 0 when calm.

The early start point is:

```
early = stop_at − climb_needed
climb_needed = net_rate × max(0, 30 min − warm_up) × 1.2      (model estimate, §4)
            = 0.5°C                                             (nothing learned yet)
start_at = clamp(early, lo, hi − 0.3 − busy_w)                 never below lo, never above hi − 0.3 − busy_w
```

- `warm_up` is the typical delay from flame to the room starting to rise (`warm_typical_s`, capped at `AT_LEARN_MAX_WARM_MS` as everywhere else).
- The 1.2 is a margin, so a run on a faster day still lasts 30 minutes. It is the same figure warm by uses for its typical estimate.
- If the heater cannot be shown to beat the loss (`net_rate` below `AT_LEARN_MIN_RATE`), `climb_needed` is 0 and `start_at` is the narrowest allowed (`hi − 0.3 − busy_w`).
- With the cabin's rates: at 0.0255°C/min and 20 minutes of warm-up, `climb_needed` = 0.0255 × 10 × 1.2 ≈ 0.31, so `early` ≈ 18.7. At 0.0069 it is about 0.08, so the 0.3 band applies: 18.7.

`auto_start_ok` (at_ctl.c) becomes: start when the reading is at or below `start_at`. Everything else around a start stays as it is: the 20 minute rest, the 30 minute minimum run, the ceiling, the restart wait for the room sensor, `room_ok`, frost and warm by.

The early start never moves the top or the working range. `lo` and `hi` keep their meaning, and the app's range display and the width learning are not affected (§6).

## 4. Where the rate comes from

Reuse warm by's model rather than building a second one:

- `at_warm_idle_loss` gives the current loss from the idle tracker (needs `AT_WARM_LOSS_MIN`, 30 minutes of idle readings).
- `at_warm_median(gross[EXT])` gives the gross heating rate (needs `AT_WARM_MIN_LEARN`, 3 rates; from 1.59 any good run gives one).
- `net_rate` = gross − loss.

When either is missing, use the halfway fallback (decision 3). Warm by's "typical" estimate (median rise without the loss) is not used here: decision 5 asked for the live figure, and the fallback covers the gap.

A small pure function, e.g. `float at_early_start_at(const at_early_in_t *in)`, in a new `components/at_core/at_early.c`, so the host tests can drive it directly.

## 5. The busy rule

Kept in the same new module, `at_early.c`, with its own small NVS blob `ctl_early` (not a new field in `at_learn_store_t`: changing that struct's version wipes all learning on every box).

- Keep the times of the last few Auto starts (thermostat starts only; not manual, timer-fixed, frost or warm by).
- **Busy:** a start makes 3 Auto starts within 2 hours. Then `busy_w` grows by 0.2°C, up to `(hi − lo) − 0.3`, at which point `start_at` is back at `lo`, as today.
- **Calm:** an Auto start that comes more than 2 hours after the previous one. Then `busy_w` shrinks by 0.1°C, down to 0.
- `busy_w` is saved, so a restart keeps it. It is cleared by `at_app_thermo_learn_reset` (it belongs to the space).

The 3-in-2-hours and step figures are first guesses. Check them against real cycles before release (§9).

## 6. Effect on Heater Care's learning

- **Width votes are unaffected.** A counted run's vote (`at_learn.c`, `r.need`, `r.climb_min`) uses its rate and the current working width, not where the run started, so an early-start run votes just as it does now.
- **Cooling times are affected.** The typical cooling time is measured from the peak to the room reaching the working bottom (`cool_ms`, kept only when `reached_bottom`). Early starts stop the room ever reaching the bottom, so no new cooling times would be kept, and the cold-spell guard (`AT_WHY_COLD_SPELL`) would run on old figures. Fix: measure cooling to the reading the run actually started at, and scale it to the working width using the idle tracker's loss, or keep the cold-spell guard on the idle tracker's loss directly. Decide during the build; the host tests must cover it.
- **Coast (stop early, 1.53)** is worked out from the working width (`at_coast_lead(hi − lo)`), so it is unchanged. Its cap of half the working range still holds.
- **Warm by** keeps its own one start inside the range (`at_ctl_allow_warm_start`). The two do not combine: a warm-by run is warm by's.

## 7. Firmware changes

- `components/at_core/at_early.c` and `include/at_early.h`: the start point (§3, §4) and the busy rule (§5), with init, restore and dirty.
- `at_ctl.c`: `auto_start_ok` uses `at_early_start_at`; record each Auto start for the busy rule; `at_ctl_auto_start_at` returns the early point so status and the apps show where it will start.
- `at_core`: init, restore and dirty for the new module. `at_app.c` `persist_ctl()`: save and load `ctl_early` beside `KEY_COOL`.
- `components/at_core/CMakeLists.txt` and `host_tests/Makefile`: add `at_early.c`.
- Cooling-time fix (§6).
- Status (`at_json.c`, `docs/API_SPEC.md`): `auto_start_at` (already in status, from `at_ctl_auto_start_at`; becomes the early point), new `thermo.early` with `start_at`, `est` (`model` / `halfway` / `off` when on the controller's sensor), `net_rate`, `busy_w`.
- Backup: add `early` (the busy state) to `thermo_learn`, as 1.58 did for the other learned state (API_SPEC §7.5). Restore must accept a backup without it.

## 8. Apps and docs

- Apps (iOS, tvOS, Android, the box's web page): where Heater Care shows "starts at", it now shows the early point, with a line saying why ("starts early: the room heats slowly" / "widened: the heater has been busy").
- Home Assistant: expose `thermo.early.start_at` as an attribute if the integration already shows `auto_start_at`.
- `docs/ADAPTIVE_THERMOSTAT.md`: point to this plan where it says runs start at or below the working bottom.
- The Tillerwell `CLAUDE.md` thermostat paragraph says "Every run starts at or below the working bottom". Update it when this ships.

## 9. Tests and release

1. Host tests (`host_tests/test_early.inc`): the start point for the model, halfway and controller-sensor cases; the clamp at `lo` and at `hi − 0.3`; the busy rule widening and calming; a restore without the blob; the cooling-time fix. Use the hsim with a slow `rate_on` (an hour-plus climb), or the minimum run hides the stop point.
2. A sim run that checks no Auto run ends inside 30 minutes, across fast and slow rates.
3. IDF build in the scratchpad copy before committing (the host compiler misses `-Werror` format checks).
4. Release as the next firmware version through the usual recipe. Chris installs from the app.
5. After a few days of real cycles: check the starts per day and the busy rule's figures against the run log, and tune.

## 10. Risks

- **More starts per day.** Limited by the busy rule. Watch the run log after release.
- **Overshoot on a fast day.** If the room heats faster than the live rate said, it reaches 19 before 30 minutes and the minimum run carries it over. The 1.2 margin and the 0.3 floor limit how far.
- **Sensor noise at 0.3°C.** The external sensor reads to 0.1°C. A noisy reading near `start_at` could start a run a little early; the 20 minute rest and 30 minute run stop that becoming short cycling.
- **Old cooling figures** until the §6 fix is in.

## 11. As built (firmware 1.60)

Code: `components/at_core/at_early.c` / `include/at_early.h`, wired in `at_ctl.c` (`at_ctl_early_point`, `auto_start_ok`, `tick_auto`), saved as NVS `ctl_early` in `at_app.c`, backup key `early` in `at_app_thermo.c`, status `thermo.early` in `at_json.c` (API_SPEC §9.13). Tests: `host_tests/test_early.inc`.

Where it differs from the plan above:

- **A fourth estimate, `gross`.** When warm by has the gross heating rate but no current idle cooling figure yet (it needs 30 minutes of idle readings after the room has fallen 0.3 below its peak, which in normal cycling often comes after the start point), the gross rate is used with no loss taken off. That is a faster climb than the real one, so a lower start: the safe side, and better than dropping to halfway every time.
- **Never less than 0.1 below the stop point.** With the early stop (coast) the stop point can sit below `hi − 0.3`; the start is capped at `stop_at − 0.1` so a run always has something to climb.
- **Rounded down to 0.1.** The cabin's fastest run (net 0.0255, 20 min warm-up) gives 18.69, so 18.6, not 18.7.
- **Warm-up not known: none assumed** (the whole 30 minutes of climb), the cautious side.
- **The busy rule counts Auto's own cycling starts only**: not Auto being switched on (by a person or a thermostat timer), not warm by's start inside the range, not fault retries. After a widening it takes 3 new starts for the next step, so one fast burst widens once rather than on every start. The start times are RAM only; only `busy_w` is saved, so the first start after a restart is never judged calm.
- **Cooling-time fix (§6):** the first option. At the next run's start, if the room never reached the working bottom, the time from the peak to the last reading is scaled to the whole fall to the bottom (`at_learn_run_start`), when the room fell at least 0.2 C and the scale is at most 5×. It applies to any run started above the bottom (early start, warm by, a manual start). Runtime fields only: the `at_learn` store is unchanged.

Known limit, seen in the simulator: on a room that heats very slowly the warm-up (a 0.2 C rise from flame) can take more than 20 minutes, and `at_learn` then gives the run no rate (`AT_LEARN_MAX_WARM_MS`), so warm by never gets a gross rate and the start stays at halfway (18.5 for 19). The cabin's runs had 15 to 49 minute warm-ups. Check the run log after release (§9 step 5): if `thermo.early.est` stays `halfway`, the warm-up cap is what to look at next.
