# Heater Care (adaptive thermostat range and run locks): plan

Status: **built (the fixes are in firmware RoamOS 1.0048, apps, Home Assistant), 2026-11-11.** Still not bench tested on the test heater (§7 step 6).

**Since 1.49 the user sets one temperature, not a range.** It is the top of the range, and the user's range is always the 1°C below it (set 20 → 19–20). Where this document says "the user's range", read that fixed 1°C. Everything else (the working range, the learning, the locks) is unchanged.

Goal: hold the room close to a steady temperature with a narrow range, and stop Hearth's own thermostat, timers and frost protection from short-cycling the heater. A person or another system can still stop a run early, and a fault or the ceiling always can. Hearth learns how fast the space heats and widens or narrows the range itself. The heater cannot be turned down (Hearth's thermostat start is `ff fb`, run indefinitely, and that does not change), so the range width and the run locks are the only things Hearth controls.

The apps call all of this **Heater Care**. The code and the API keep the plain names used below (`thermo`, `at_learn`, `learned_width`).

The 30 minute minimum run and 20 minute rest are Hearth's own control choice. They are not a figure from Autoterm, and nothing in the apps or docs should present them as one. Autoterm's only related figure is "at least 20 minutes once a flame has been established", from its maintenance guidance.

## 1. Decisions (agreed 2026-11-11, updated the same day after review)

| Topic | Decision |
|---|---|
| Default range | **1°C.** On the Comfort Controller's own sensor (whole degrees) the user's range may still be 1°C, but Hearth runs on at least **2°C**. |
| Name | **Heater Care** in the apps, for the learning, the locks and the warnings together. |
| Existing settings | **No migration.** Chris is the only user, for testing, so nothing is moved on the update. The new default is 20–21 on a fresh box. Ranges and timers already saved on the test boxes are left as they are and changed by hand. |
| What the user sets | The user's range is what they see as their setting. Learning only ever lowers the bottom below it. It never moves the top and never narrows below the user's range. |
| How the width is learned | Each counted run measures how fast the room rose and works out the width that would take about **40 min** to climb, counted **from flame established**, the same starting point as the minimum run. That leaves 10 min between the 30 min minimum run and reaching the top. When **2 of the last 3** counted runs agree the width should change, it **goes straight to that width** (the wider of the two, rounded up to 0.5°C, or to a whole degree on the controller's sensor), however big the jump: 3°C to 1°C in one move if the runs say so. |
| Fast or slow | Decided by **heating speed only**, not the clock. From the measured speed, Hearth works out how long the current range takes to climb from flame established. Under **30 min** is fast, over **60 min** is slow, in between means no change. |
| Widening | Fast runs work out a wider width. Cap: **4°C** total width. |
| Narrowing | Slow runs work out a narrower width. Never narrower than the user's range. Runs after an unusually fast cool-down do not count (cold-night guard). |
| Sensor drops out | While on the controller's whole-degree reading, the working range is at least 2°C. Back to the learned range when the external sensor returns. **Learning continues** on both sensors. |
| Learning and the two sensors | **Each sensor keeps its own last 3 runs**, so a decision is only ever made from runs measured on the same sensor. The learned width is still **one number for the space**, changed by whichever sensor is in use. |
| Stuck sensor | If the **external** sensor's reading has not moved at all for **15 min** while the heater is giving heat, Hearth treats it as failed and switches to the controller's reading. Not applied to the controller's whole-degree sensor, which often sits still. |
| Minimum run | **30 min from when the flame is established** (the heater reports it is burning), not from switch-on. Light-up time comes on top. Hearth's own stops (thermostat, timer end, lost readings) are held until the minimum run is up, even if the room goes over the top. A heater fault, reaching the temperature ceiling, or a stop the user asks for always takes effect at once. |
| Frost protection minimum run | **20 min from flame established.** Shorter than the thermostat's, so an empty space is not heated far past its frost target. |
| Ceiling | **Top of the range + 5°C** (for frost protection, its stop point + 5°C). If the room reaches it, Hearth stops the heater even inside the minimum run. A backstop for a very small space or something gone wrong. It should never fire in normal use. |
| Minimum off | **20 min** from when the heater is back to idle. Only frost protection may start a run sooner. |
| Restart during the rest | Hearth **saves the time of each stop**, so the 20 min rest carries on correctly after a power cut or update. If the clock is not set at boot, the rest counts 20 min from boot. |
| Timer ends inside the minimum run | **The run finishes its minimum run**, then stops. A timer can overrun its end by up to 30 min plus light-up. The shortest timer goes up from 20 to **30 min**. |
| Thermostat turned off, or range lowered, inside 30 min | **Warns like Stop.** "Let it finish" turns the thermostat off (or applies the new range) and lets the run reach 30 min. "Stop anyway" stops at once. |
| Every temperature reading lost mid-run | The run **finishes its minimum run, then stops** (asked again after review, kept). No new run starts until a reading is back. The apps show why. The ceiling cannot work without a reading, so for that time only the heater's own coolant limit applies. |
| Power cut mid-run | If Hearth restarts and finds the heater idle after a run that never recorded a stop, it rests **20 min from boot**. |
| Heater pauses its own burner | When the coolant gets hot the heater pauses its burner and relights by itself. That is **one run**, not a stop and a new start. The locks carry straight through it, and the learning counts it as part of the real time the room took to heat. |
| User Stop inside the minimum run | Always works. The apps warn first, with a short reason and a "Stop anyway" button. |
| Stop from places that cannot warn (HomeKit/Matter, Home Assistant/MQTT, the device web UI's API, the Comfort Controller) | Stops at once, is logged, and the short-cycle explainer shows the next time an app opens. |
| Manual Start/Stop (not thermostat) | Not locked. The apps warn if it would short-cycle: a stop before the end of the minimum run, or a start inside 20 min of the last stop. The stop warning uses the same deadline as the lock: it shows all through light-up (the full 30 min is still to come), counts down from flame established, and stops showing once the 30 min is up. |
| What learning belongs to | **The space.** Changing the range, or a timer switching ranges, keeps it. Only a **Reset learning** button clears it. It goes in the backup, so **a replacement box restored from the cloud gets it back**. |
| Cooling | **Recorded and explained, never moves the range itself.** Each off period's cooling is logged. It explains dips below the range, and it stops a slow run on an unusually cold night counting towards narrowing. |
| Off switch | **None.** Learning is always on. |
| Where it shows | **iOS, Android, tvOS, device web UI.** In-app only, **no push notifications**. |
| App wording | The warnings give the reason as Hearth's own choice ("heaters like this last longer with fewer, longer runs"). No flat claims about glow plugs or carbon. |
| Release | Firmware and apps go out **the same day**, so they match. |

## 2. Terms

- **User range:** `auto_low`–`auto_high`, as set by the user. Minimum width 1°C on either sensor.
- **Learned width:** a stored number, 0 to 4°C in 0.5°C steps. 0 means "no widening learned yet". It is the real swing the space needs, so it means the same on both sensors, whichever of them set it. On the controller's sensor it is used rounded up to a whole degree plus 1°C (see Working range), and that 1°C is never saved.
- **Working range:** what the thermostat actually runs on. Top = `auto_high`. Width = the largest of the user range, the learned width, and 2°C when on the controller's sensor. On the controller's sensor the learned width counts as itself rounded up to a whole degree, plus 1°C, when anything has been learned, because the room there really swings the working width less 1°C (§3.2): learned 1.5°C runs on 3°C there, learned 1°C on 2°C. Capped at 4°C unless the user's own range is wider. Bottom = top − width.
- **Ceiling:** `auto_high` + 5°C.
- **Run:** from the heater turning on (state leaves idle after a start) to the heater being idle again. A burner pause and relight by the heater itself is part of the same run.
- **Flame established:** the first time in a run that the heater reports it is burning, after light-up.
- **Minimum run:** 30 min from flame established for the thermostat and the manual warnings, 20 min for frost protection. Until the flame is established the minimum run has not started counting, so the lock is on in full.

## 3. Firmware (`components/at_core`, `components/at_app`)

### 3.1 Control loop: `at_ctl.c` / `at_ctl.h`

- **Room reading filter:** the room reading used by the thermostat, frost protection, the ceiling and the learning is the middle value of the last 3 readings from the sensor in use. One bad reading then cannot start or stop a run, trip the ceiling, or upset the learning. The filter restarts when the sensor changes. First check what filtering the sensor code already does, so it is not done twice.
- `tick_auto` uses the working range in place of `low`/`high`. The rule "a run only ends at the top" stays.
- **Minimum run lock:** record `run_on_ms` when the heater leaves idle and `flame_on_ms` when it first reports burning in that run. Hearth's thermostat stop is held back until `now - flame_on_ms >= 30 min`, and for the whole of light-up before that, even if the room is above the top. Confirm in `docs/PROTOCOL.md` which `state_raw` value means the flame is established. Both times use the uptime clock, never the wall clock, so a clock change cannot shorten a lock. The note says so: "at 22.4, above 21.0, holding: minimum run 8 min left".
- **What the run lock holds back:** every stop Hearth would send by itself. That is the stop at the top, the stop when a timer ends (§3.4), the stop when the thermostat is turned off or the range is lowered with "Let it finish", and the stop after every temperature reading is lost.
- **What the run lock never holds back:** a fault, the ceiling, and a stop the user asked for.
- **A stop the heater did not act on:** a stop Hearth sends (its own, a held one, frost protection's, the ceiling's, "Stop anyway" on thermostat off or a lowered range, or a Stop someone asks for through Hearth from an app, the web UI, Matter, MQTT/HA or the console) is sent again every 90 s (`AT_CTL_MIN_ACTION_MS`) while the heater still reports `0x02`/`0x03`, until it purges (`0x04`/`0x05`) or goes idle. The ceiling still fires while a stop is outstanding: it sends the stop at once instead of waiting for the next retry. A start someone asks for in the meantime (the Comfort Controller, a start through Hearth, or Auto being turned on and taking the run over) ends the retries, and the run carries on as theirs. If the heater relights from its purge (`0x04` back to `0x02`/`0x03` without reaching idle) after such a start, it is the same run: the locks keep counting from its first flame, and Auto stops it at the top as usual. A run broken like this is never counted for learning ("not_at_top"). If the heater purges to idle anyway, the run ends as the stop that was sent.
- **Stuck sensor (1.50+):** while the heater is burning, the external sensor is marked failed and Hearth switches to the controller's reading, the same as a dropout, only when its raw reading has not changed at all for at least 15 min **and** the controller's own reading has risen by 2°C or more over that same time. A flat reading alone is never enough: a room that holds steady through a slow heat-up is real, and before 1.50 that alone set the sensor aside (a room at 16.9°C was swapped for a controller reading 14°C). It is trusted again on the first reading that differs from the stuck value; re-marking it needs another 15 min and another 2°C climb, so it cannot flip back and forth quickly. Not applied to the controller's sensor, and never while the controller has no fresh reading. Logged. The first check on any sensor stays the age of its last reading: a sensor that has gone quiet is dropped by its age, and the stuck check is only for one that keeps sending the same number. The age limits are the ones the firmware already uses and do not change: the external sensor is dropped when its last reading is over 90 s old (`AT_CTL_EXT_STALE_MS`), and the controller's reading when it is over 10 s old (`AT_CTL_MAX_READING_AGE_MS`, the controller reports every few seconds).
- **Ceiling:** if the room reaches `auto_high + 5` (frost protection: its stop point + 5), stop at once, lock or not. Logged with its own reason, and shown in the apps. The run does not count towards learning.
- **No reading:** if every temperature reading is lost mid-run, the run carries on to the end of its minimum run and then stops. No thermostat run starts while there is no reading. The ceiling cannot fire without a reading, so the end of the minimum run is the limit here.
- **Minimum off lock:** record when the heater reaches idle. The thermostat start is held back until 20 min have passed: "at 19.6, below 20.0, waiting: minimum off 6 min left".
- **Stop time kept over a restart:** the wall-clock time of each stop is written to NVS (one write per run). At boot, if the clock is set, the off lock carries on from the saved time. If the clock is not set, the off lock counts 20 min from boot. A stop made while the clock was not set is still saved, as "stopped, time unknown" (`have_stop` 1, `stop_epoch` 0), and the next boot rests 20 min from boot whatever the clock says then. A box that has never run owes no rest. If the clock is set later, during that rest, the lock switches to the saved time, which is the true rest and may end it sooner. The saved time is only trusted when it makes sense: if it is in the future, or the clock reads earlier than the firmware's build date, it is ignored and the 20 min from boot stands. The saved time is used once only: Hearth works out how much rest is left and turns that into an uptime deadline. After that the off lock runs on the uptime clock like the run lock, so later wall-clock changes cannot move it.
- **Power cut mid-run:** a "run in progress" mark is written to NVS when a run starts and cleared with the stop time when it ends (two writes per run). If Hearth boots, finds the mark still set and the heater idle, the run ended without Hearth seeing it, so the off lock counts 20 min from boot.
- **Frost protection (`tick_freeze`):** gets a 20 min minimum run from flame established. Its stop at `target + 1` waits for the lock (`holding: "top"` while it does). It is the only loop allowed to start inside the 20 min off lock.
- **Frost protection and Auto together:** when Auto is on and would start (or has a start waiting to show), frost protection sends nothing. Frost protection starts alone only when Auto is off or held back by the off lock. Any run going while Auto is on belongs to Auto, including one frost protection started: 30 min lock, Auto's ceiling, stopped at the top of Auto's range, never at the frost target + 1.
- **Timer ended, run held:** the ceiling stays the timer's top + 5 while the held run finishes, though the user's own range is put back at once.
- **Heater's own burner pause:** the heater pauses its burner on coolant temperature and relights by itself. The run start time and the locks carry straight through it. Check in `docs/PROTOCOL.md` and the heater logs which `state_raw` values a pause reports, and make sure none of them is read as idle. `at_ctl.c:199` treats only `0x00` as idle today.
- The first-run special case (`set_temp`, start at `high - 1`) goes. With a 1°C default it is the bottom anyway, and wider working ranges come from learning, which must start at the real bottom.
- Lock timing is shared between both loops and manual control. Locks are measured from what the heater reports (`state_raw`), not from what Hearth sent, so a run started from the Comfort Controller or Home Assistant counts too.
- Boot: the current "restored mid-cycle, wait for low" behaviour stays. If the heater is already running at boot, the run keeps its true age (1.51): the wall-clock time the flame was established is saved to NVS when it lights, and restored at boot (or once the clock is set), so a restart does not give the run a fresh 30 minute lock. Only when that time is not known (no clock when it lit, an update from 1.50 or earlier, a saved time that makes no sense) does the run lock count from boot, the safe side, and `thermo.run_adopted` is true so the apps can say so.

### 3.2 Learning: new `at_learn.c` (in `at_core`, pure C, host-testable)

Per run, record: start temperature, time on, time the top was first reached (or none), how the run ended (top reached, user stop, fault, ceiling, heater stopped itself), and which sensor was in use.

A run **counts** towards learning only if:
- Hearth's thermostat started it, and the heater had been off for **less than 2 hours** before it. That leaves out the first run from a cold space, after boot or a long time off. How far the room dipped below the bottom does not matter.
- It ended normally: top reached and Hearth's stop after the lock. Runs stopped by the user, ended by a fault or the ceiling, or stopped by the heater itself are logged but not counted.
- The sensor did not change during the run.
- The room did not lose heat part-way through (an open door or window). A run is left out if the room falls 0.5°C or more below its highest reading after the warm-up delay, or if the rise rate comes out below 0.005°C per minute (which covers zero and falling). Logged with the reason "lost heat mid-run, not counted". This also keeps a zero or near-zero rate out of the climb time sum. All of this works on the filtered reading (§3.1), so one bad reading does not rule a run out.
- There was enough to measure. A run is left out if its warm-up delay was over 20 min, or if fewer than 10 min of readings (or fewer than 2 tick-overs on the whole-degree sensor: the warm-up tick-over and one more, which is what a normal run over a 2°C working range gives) were left for the fit. Logged with the reason "not enough data". The warm-up delay used in the sums is therefore never more than 20 min, so the width needed can never come out at zero or below.

**Working out the width from each counted run:**
- **Every time in the learning is counted from flame established**, not from switch-on, so it lines up with the minimum run. Light-up is left out of all of it: a 5 min or a 15 min light-up gives the same answer.
- **Warm-up delay:** from flame established to the room's first clear rise. The baseline is the lowest filtered reading **from flame established onwards**, starting with the reading at that moment. Readings from light-up are never used. The delay ends when the filtered reading is 0.2°C above that baseline, or at the first whole-degree increase on the controller's sensor. Taking the lowest reading, not just the first, covers a room that is still cooling for a few minutes after the flame is established. It is the time the coolant takes to warm and reach the room. If the room is already rising when the flame is established, the delay is 0. Measured each run, and a typical value (median of the last 10) is kept **for each sensor**, since the two sensors see the first rise differently and may be in different places. The sums always use the typical value of the sensor in use.
- **Rise rate (°C per minute):** a straight-line fit of the room readings from the end of the warm-up delay to the moment Hearth sends its stop. A fast run keeps going past the top until its minimum run is up, so the fit covers all of the burning, not just the climb to the top. Readings after the stop are never in the fit: the room can keep rising through the purge from the warm coolant, and that belongs to the cooling record below, not to the heating speed. The fit runs on real elapsed time. If the heater pauses its own burner part-way, that time and its readings stay in: the pump still moves warm coolant, and the 40 min target is the real time the room takes to climb, pauses included. Nothing is cut out of the time line.
- **Real swing on the controller's whole-degree sensor.** The start fires as the reading falls into the bottom degree (the room just under bottom + 0.5°C) and the stop as it enters the top degree (the room at top − 0.5°C), so over a working width W the room really climbs W − 1°C: 1°C on a 2°C range. The climb time below uses that real swing on the controller's sensor, and the full working width on the external sensor.
- **Width needed** = rise rate × (40 min − typical warm-up delay), on either sensor: the real swing that takes 40 min. The controller's extra 1°C is not part of it; it is added only when the controller's working range is worked out (§2), never saved, so a width learned on one sensor is right on the other. Example on the external sensor: 0.1°C/min with a 6 min delay → 3.4°C, which rounds up to 3.5°C. 0.03°C/min → 1.02°C, which rounds up to 1.5°C. On the controller's sensor, 0.04°C/min with a 2.5 min delay → 1.5°C, which that sensor runs on as 2 + 1 = 3°C (18–21 for a 20–21 range), and the external sensor as 1.5°C (19.5–21). Rounding is up to the next 0.5°C when widening (see Deciding), and the 2°C floor, the 4°C cap and the whole-degree working width still apply.
- **Climb time for the current range** = typical warm-up delay + real swing ÷ rise rate, counted from flame established; the real swing is the working width on the external sensor and the working width − 1°C on the controller's. This is worked out from the speed, not read off the clock, so it is the same wherever the run started. Example: on the controller's sensor at 19–21, 0.04°C/min and a 2.5 min delay climb in 27.5 min (fast), not the 52.5 min the full 2°C would suggest; after widening to 18–21 the same room climbs in 52.5 min (no change).
- **Fast run:** climb time under 30 min. Its width needed is always wider than the current one.
- **Slow run:** climb time over 60 min. Its width needed is always narrower than the current one.
- A run that is neither fast nor slow means the current width is right. It counts as a vote for "no change".

**Deciding:**
- Keep the last 3 counted runs and their width needed, **separately for each sensor**. A decision only ever uses the window of the sensor in use, so runs measured on the external sensor and on the controller's are never judged together. The learned width they change is one number for the space.
- If 2 of the 3 are fast, or 2 of the 3 are slow (after the cold-night guard below), set the learned width to **the wider of those two runs' width needed, rounded up to the next 0.5°C** (a narrowing on the controller's sensor rounds up to a whole degree, below). Clamp to the limits: no narrower than the user range on the external sensor, no wider than 4°C, and no wider than 3°C from the controller's sensor's runs (its working width, 3 whole degrees plus 1°C, is then at the 4°C cap).
- **The controller's sensor taking over never raises the learned width.** The learned width is what the space needs, as best measured. The 2°C working floor belongs to the controller's sensor and is applied when the working range is worked out (§2), not saved. So if the external sensor has learned 1°C and the controller's sensor takes over, Hearth runs on 2°C but the learned width stays 1°C, ready for when the external sensor returns. The same goes for the controller's extra 1°C (real swing = working width − 1°C): it is applied where the controller's working range is worked out, never saved. The learned width is always the real swing, so after the controller's sensor has learned, the external sensor's runs come out neither fast nor slow, and the other way round: one sensor never undoes the other's width.
- **The coarser sensor cannot narrow below what it can see.** In real-swing terms its 2°C working floor is a learned width of 1°C. A narrowing decision on the controller's sensor rounds its width up to a whole degree and never takes the learned width below 1°C. If the learned width is already 1°C or lower, slow runs on the controller's sensor neither push it up to 1°C nor take it lower: it is left exactly as it is (1°C or 0), and that is the controller's `"min"` limit.
- On the controller's sensor the working width is the learned width rounded up to a whole degree, plus 1°C, since the sensor cannot see half a degree and the room swings 1°C less than the range there.
- The new width can be any distance from the old one (3°C to 1°C in one move, or 1°C to 2.5°C). Rounding towards wider is deliberate: too wide only means slightly longer runs and more swing, too narrow means overshoot and more runs to recover.
- Aiming at 40 min, with fast at 30 and slow at 60, leaves room either side, so a normal run at the new width does not tip it into changing again.
- After any change, clear both windows so the next decision uses runs at the new width.
- **At a limit:** if the new width, after rounding and the limits, is the same as the current one, the learned width is not updated and the windows are not cleared. Hearth notes the state once as `at_limit` = `"max"` (the runs want more than the 4°C working cap) or `"min"` (the runs want less than the user's range, or less than 2°C on the controller's sensor), with one history entry the first time, "limit reached". If rounding alone keeps the working width where it is while it is above its floor, that is no change and not a limit. What is saved, and when:
  - **Each sensor has its own limit state** (`"max"`, `"min"` or none), kept beside its own 3-run window. A limit state always means the same thing: the working width on that sensor is sitting at that limit, and this sensor's runs want to go past it. For `"max"` that is the shared learned width at 4°C on the external sensor, or at 3°C or more on the controller's sensor (where its working width is at the 4°C cap). For `"min"` it is the working width at its floor: the user's range, or 2°C on the controller's sensor (a learned width of 1°C or less), even when the stored learned width is lower. It is never a loose recommendation. So both sensors can hold `"max"` together, or `"min"` together, but with a user's range under 3°C one cannot be at `"max"` while the other is at `"min"`: the one shared width cannot sit at both ends. (If a sensor wants narrower while the width is at the cap, that is not a limit, it simply narrows the width.) The status reports the state of the sensor in use.
  - On first reaching a limit: save that sensor's limit state and one history entry, which names the sensor.
  - Later decisions at the same limit: no write and no history entry.
  - When the state clears: save that, with a "limit cleared" history entry.
  - If the limit is reached again later: a new limit event, saved and logged like the first.
- **What clears a sensor's limit state:** that same sensor's latest 3 counted runs no longer asking to go past the limit, or a decision from that sensor changing the learned width. Runs and results from the other sensor cannot clear it.
- One case is not a result from the other sensor but a plain fact: if the learned width is moved off the limit value by the other sensor's decision, a state of "cannot go further" is no longer true. So a `"max"` state is dropped when the learned width is no longer at that sensor's cap (4°C, or 3°C on the controller's sensor), and a `"min"` state when that sensor's working width is no longer at its floor, whoever moved it. That sensor's next decision is then taken afresh, and it can reach the limit again as a new event.
- `"min"` is the everyday state for a large or slow space on a 1°C range. It is not a problem and the apps say nothing about it. `"max"` means the space heats faster than the widest range allows, so runs may go over the top: the "Why?" screen says so.
- The history entry records the old and new learned width (the real swing), the width as worked out before rounding, the working width on the sensor in use (on the controller's sensor the learned width rounded up to a whole degree plus 1°C, so it shows the extra 1°C), the sensor, and the two rise rates that caused it, for the "Why?" screen. Example: worked out 2.1°C, learned 2.5°C, running on 4°C on the controller's sensor.
- On the whole-degree sensor, a rise only shows when the reading ticks over, so the fit uses the times of each tick-over. The rate is coarser and a change may come a run late. It still counts (decision: keep learning).

**Cooling.** Per off period (Hearth's stop at the top to the next start), record:
- the highest temperature the room reached from the stop on, purge included, and when. The room can keep rising through the heater's purge and for a while after it, from the warm coolant and radiators. That peak is the true top of the cycle;
- the time from that peak to the room first reaching the working bottom (or none, if it never did before the next start). This is the cooling time;
- the lowest temperature during the off period, and how far below the bottom the room fell while the 20 min off lock was holding the start back.

Cooling never widens or narrows the range by itself (it moves with the weather, doors and time of day, so it is too noisy). It is used in two ways:
- **Narrowing guard:** keep a running typical cooling time (median of the last 10 recorded off periods) for each sensor. A run is compared with the typical time of the sensor it was measured on. A slow run does not count as slow if the off period before it cooled to the bottom in under half the typical time. A cold night then cannot narrow the range by mistake. Those runs are logged with the reason "cold spell, not counted".
- **Explaining dips:** when the room fell below the bottom during the off lock, the run log says so, for the apps' status line and "Why?" screen.

The typical cooling time is also what a later "warm by a set time" feature for timers will need (§9), so it is stored from the start.

Store in NVS (one blob, versioned): learned width, the 3-run window for each sensor, the last 10 warm-up delays and the last 10 cooling times for each sensor, the limit state of each sensor, and a history ring of the last 20 changes and events (width changes, "limit reached", "limit cleared", resets), each with its time, sensor, reason, the widths, and the rise rates that caused it. Write only when something changes, never every tick.

### 3.3 Settings and the hysteresis fields: `at_app.c`, `at_settings.c`

- Today `hyst_min` is clamped 2–7 and `hyst_max` 1–3, the default is 2 + 1 (`at_app.c:285-289`, `621-628`), and `at_app.c:543`/`608` require `range >= hyst_min + hyst_max`.
- New: the minimum user range is 1°C on either sensor. `hyst_min`/`hyst_max` become **read-only compatibility fields**, always reported as 0.5 + 0.5, so older apps that add them up get a 1°C minimum. (iOS and tvOS read them as decimals and round the sum up, so this works with today's builds. Check Android does the same.) The `hysteresis` command is accepted and ignored with `"skipped"`, and older apps keep working.
- **No migration.** The default range on a fresh box becomes 20–21 (`at_app.c:288`, now 19–22). A range already saved on a box is loaded as it is. Learned width starts at 0.
- `auto_target` stays the middle of the user range.

### 3.4 Timers: `at_timers.c` / `at_timers.h`

- Timer ranges follow the same rules: minimum width 1°C, and the learned width and the 2°C controller-sensor floor widen them at run time just like the main range.
- `min_gap_c` (`at_timers.h:94-97`), now `hyst_min + hyst_max`, becomes 1°C.
- **Shortest timer: 30 min** (up from 20), so a timer that runs the heater once never asks for a short run. The rule applies when a timer is created or edited. Timers already saved are left alone.
- **Timer ends inside a run's 30 min:** the timer's stop is held until the run lock is up, then sent. The status says "Timer ended, finishing minimum run: 20 min left". If the next timer has started by then, it takes over the run as normal.
- The 30 min gap between timers (`AT_TIMER_GAP_MIN`) stays. It lines up with the minimum run. An overrun can last a little longer than the gap when light-up was slow, and the next timer then takes over the run.

### 3.5 Status, events and backup: `at_json.c`, MQTT, `docs/API_SPEC.md`

New fields in `/api/status` (and the MQTT state):

```json
"thermo": {
  "work_low": 19.5, "work_high": 21.0,
  "learned_width": 1.5, "min_width": 1.0, "sensor": "external",
  "ceiling": 26.0,
  "at_limit": null,             // "max" | "min" | null: on the sensor in use, learning wants to go past a limit and cannot
  "reason": "widened",          // "user" | "widened" | "narrowed" | "controller_sensor"
  "min_run_left_s": 480,        // 0 when no run lock; the full 1800 (1200 for frost) all through light-up; the apps use this for the stop warning
  "min_off_left_s": 0,          // 0 when no off lock
  "holding": null,              // why a stop is waiting for the run lock: "top" | "timer_ended" | "thermostat_off" | "no_reading" | null
  "run_on_at": 1762871100,      // epoch of the current run's start, or null
  "flame_on_at": 1762871400,    // epoch the flame was established in this run, or null while lighting up
  "sensor_stuck": false,        // true while the external sensor is set aside as stuck
  "no_reading": false,          // true while there is no usable room reading (the run stops after its minimum run)
  "rise_rate": 0.05,            // °C per minute from the last counted run, or null
  "warmup_typical_s": 360,      // median time from flame established to the room's first rise, or null until known
  "cool_typical_s": 2700,       // median time from the peak after a stop to the working bottom, or null until known
  "dip_below": 0.8,             // how far below the bottom the room fell during the current/last off lock, 0 if none
  "early_stop_unseen": 1,       // early stops the apps have not yet explained
  "last_run_end": "top",        // how the last run ended (list below), or null
  "last_early_stop": {"at": 1762871940, "after_s": 540, "source": "matter"}  // the newest early stop, or null
}
```

`min_width` is the narrowest working range on the current sensor (1.0 external, 2.0 controller). The user's own range may be 1°C on either. `rise_rate`, `warmup_typical_s` and `cool_typical_s` are the values for the sensor in use.

- `GET /api/thermo/history`: the learning history ring and the last 20 runs (with whether each counted and why not, and the cooling of the off period before each), for the apps' "Why?" screen.
- `POST /api/cmd {"cmd":"thermo_learn_reset"}` (manager): clears the learned width and history, logged.
- `POST /api/cmd {"cmd":"early_stop_seen"}` (control): the app has shown the explainer.
- **Turning the thermostat off or lowering the range mid-run:** the command takes `"finish_run": true|false`. `true` applies the change and lets the run reach 30 min. `false` stops at once and is logged as an early stop. Older apps send neither, which is treated as `true`.
- **Early-stop log:** every stop before the end of the minimum run (light-up included) is recorded with its source (app, Matter, MQTT/HA, web, Comfort Controller/unknown) and raises `early_stop_unseen`. Only stops that a person or another system asked for go here.
- **Ceiling stops and faults are kept apart from early stops.** They are Hearth or the heater protecting itself, not a short cycle someone caused, so they never raise `early_stop_unseen` and never show the short-cycle explainer. Each is in the run log with its own reason (and the fault code), and `thermo.last_run_end` reports how the last run ended (`"top"`, `"user"`, `"external"`, `"ceiling"`, `"fault"`, `"no_reading"`, `"heater"`, `"timer_ended"`, `"thermostat_off"`), so the apps can show the right line.
- **Backup (`GET/PUT /api/backup`, §7.5):** add `"thermo_learn": {"width":1.5,"window_ext":[…],"window_ctl":[…],"warmup_ext":[…],"warmup_ctl":[…],"cooling_ext":[…],"cooling_ctl":[…],"limit_ext":null,"limit_ctl":null,"history":[…]}`. `PUT` restores it, so a replacement box restored from the cloud starts with what the old one learned. The backup `v` stays 1 (the field is optional and older boxes ignore it).
- API_SPEC.md is updated in the same change, including the deprecation of `hysteresis`.

### 3.6 Host tests: `host_tests/`

Cases to cover before any hardware test:
- Minimum run holds a stop past the top. Minimum off holds a start below the bottom. Frost overrides the off lock and keeps the run lock.
- User stop inside 30 min stops at once and is logged as an early stop with its source.
- Ceiling: the room reaching top + 5°C stops the heater inside the 30 min, is logged, and the run is not counted. It does not raise `early_stop_unseen`, and neither does a fault. A stop from Matter does.
- Warm-up baseline: a room that changes during light-up, or keeps cooling for 3 min after flame established, still gets the right warm-up delay.
- Manual stop warning: `min_run_left_s` is the full minimum run during light-up, counts down after flame established, and is 0 after the deadline.
- Learning is counted from flame established: the same run with a 5 min and a 15 min light-up gives the same warm-up delay, width needed and climb time.
- A fault inside 30 min is never held back.
- Timer ends 10 min into a run: the stop waits until the minimum run is up, then goes. A new or edited timer shorter than 30 min is refused. One already saved still loads.
- Minimum run counts from flame established: a run with a 12 min light-up is held for 42 min from switch-on. A stop asked for during light-up is held too.
- Frost protection reaches target + 1°C after 12 min: it stops at 20 min from flame established, not 30. Its ceiling is its stop point + 5°C.
- Stuck sensor: the external reading sits at exactly 20.0°C for 15 min while burning → Hearth switches to the controller's reading. The same on the controller's sensor does nothing. A reading that then moves is trusted again.
- Sensor disappears 2 min into a run: the run carries on to the end of its minimum run, then stops.
- External sensor drops out and returns several times: each run goes into its own sensor's window, and no decision mixes the two.
- Power cut mid-run: boot with the "run in progress" mark set and the heater idle → 20 min rest from boot. Boot with the heater still running → run lock from the saved flame time, or from boot when that is not known (`run_adopted`).
- The off lock starts when the heater reports idle, after its purge, not when the stop was sent.
- Wall clock jumps backwards or forwards mid-run or mid-rest: neither lock changes length. The same for a rest restored from the saved stop time after a restart: once the deadline is set, clock jumps do not move it.
- Warm-up and cooling medians are kept per sensor: runs on the controller's sensor do not change the external sensor's typical values, and the other way round.
- Readings after Hearth's stop (the room still rising through the purge) do not change the rise rate.
- The reading that makes Hearth stop at the top is part of the fit: a run on the controller's sensor that takes over 30 min from flame to reach the top (two tick-overs, the second on the stop tick) is counted and votes slow or ok.
- A stop someone asked for that is lost on the bus is sent again every 90 s until the heater purges. A start asked for after a stop (or Auto turned on) clears the stopping state; a heater that relights from its purge is the same run, locks unbroken, and Auto stops it at the top.
- A run with a 25 min warm-up delay, or with under 10 min of readings to fit, is not counted. The width needed never comes out at zero or below.
- Stuck sensor: a room reading that stays flat for an hour of burning while the controller's reading also stays flat is never set aside. Flat for 15 min while the controller climbs 2°C is. The first new value brings it back.
- Old app on new firmware: `hyst_min + hyst_max` reads as 1, the `hysteresis` command is skipped without an error, thermostat-off without `finish_run` lets the run finish.
- Thermostat off and range lowered mid-run, with `finish_run` true, false and missing.
- Every reading lost mid-run: runs to 30 min, stops, no new start until a reading is back.
- Heater's own burner pause and relight: one run, locks unbroken, and the pause stays in the rise-rate fit as real elapsed time.
- Restart during the off period: with the clock set the rest carries on from the saved stop time, without it the rest counts 20 min from boot. Clock set part-way through the rest: the lock switches to the saved time. Saved time in the future, or a clock before the build date: ignored.
- Stop from the Comfort Controller mid-run: the 20 min rest starts, it is logged as an early stop, and Hearth does not start the heater again inside the rest.
- Open window: a run where the room falls 0.5°C or more after it started rising, or with a rise rate below 0.005°C per minute, is not counted and does not break the sums.
- One bad reading (0.6°C low, or 6°C high, for a single reading): no start, no stop, no ceiling stop, and the run still counts.
- Cooling is timed from the peak after the stop, not from the stop itself.
- Rise rate fit, warm-up delay and width needed from recorded readings, including runs that went past the top, and on whole-degree tick-overs.
- Fast and slow come from the speed: the same speed gives the same answer whether the run started at the bottom or 1.5°C below it.
- Widening: fast, fast → jump to the wider of the two widths needed, rounded up. Fast, slow, fast → widen. One fast run alone → no change. Cap at 4°C.
- Narrowing: 2 slow out of 3 → jump down. A big change (3°C to 1°C) happens in one move. Two runs saying 1.2°C and 0.9°C → 1.5°C. Never below the user range, never below 2°C on the controller's sensor.
- At the 4°C cap with runs asking for 5°C: no change, `at_limit` is `"max"`, one "limit reached" history entry, and nothing more is added by the next 6 fast runs. The same at the bottom with slow runs (`"min"`).
- Limit life cycle: reach the 4°C cap (one history entry) → 3 counted runs that no longer ask for more → `at_limit` clears, with a "limit cleared" entry → fast runs again → a new limit event is recorded. Each sensor's limit state survives a restart and a backup round trip.
- Limit reached on the controller's sensor is not cleared by runs on the external sensor, and the other way round.
- Both sensors reach `"max"` in turn: each is kept with its own history entry, and the status shows the one for the sensor in use. External at `"max"`, then the controller's runs ask for narrower: the width narrows, the external `"max"` is dropped, and there is never a `"max"` and a `"min"` held together.
- The other sensor's decision moves the learned width off the cap: the first sensor's `"max"` state is dropped, with a "limit cleared" entry.
- History entry on the controller's sensor shows worked out 2.1°C, learned 2.5°C, working 4°C.
- One learned width for both sensors, through the control loop (room 19.4°C, 0.04°C/min heating and cooling, range 20–21): the controller's sensor learns 2°C and runs on 18–21; the external sensor takes over and runs on 19–21, its runs vote ok and nothing changes; the controller's sensor takes over again, 18–21, its runs vote ok and nothing changes.
- External sensor learns 1.5°C: working width 1.5°C on it, 3°C on the controller's sensor.
- Nothing learned, user range 1°C: working width 2°C on the controller's sensor.
- Learned width 3°C, fast runs on the controller's sensor: learned width stays 3°C, the controller's limit state is `"max"`. Learned width 2°C, the same runs: 3°C, not more.
- External sensor learns 1°C, then the controller's sensor takes over: working width 2°C, learned width still 1°C. Two slow runs on the controller's sensor asking for 1.2°C: learned width still 1°C, the controller's limit state is `"min"`. External sensor returns: working width 1°C again.
- Learned width 3°C (working 4°C on the controller's sensor), controller's runs ask for 1.2°C: learned width goes to 2°C, not 1.5°C, working width 3°C.
- Learned width 1.5°C (working 3°C on the controller's sensor), slow runs there asking for under 1°C: learned width goes to 1°C, working width 2°C; more slow runs leave it there, `"min"`.
- Settling: after a change, a run at the measured speed comes out neither fast nor slow.
- The working width on the controller's sensor: the learned width rounded up to a whole degree plus 1°C, at least 2°C, at most 4°C unless the user's range is wider.
- Excluded runs: more than 2 hours off before it, user stop, fault, ceiling, sensor change mid-run, heat lost mid-run.
- Cooling: off period times and dips recorded; a slow run after an off period that cooled in under half the typical time is not counted towards narrowing; cooling alone never changes the width.
- Sensor drop mid-run: working range jumps to at least 2°C, returns afterwards.
- No migration: a saved 18–21 range loads unchanged. A fresh box defaults to 20–21, and its working range is 19–21 on the controller's sensor.
- Backup round trip keeps the learned width and history.
- Boot with the heater already running: run lock counted from boot.

## 4. Apps

The wording below is a draft for Chris to adjust. Short, plain, no jargon. The feature is called **Heater Care** wherever it needs a name: the "Why?" screen title, the explainers, and the lock lines ("Heater Care: minimum run, 8 min left"). Reasons are given as Hearth's own choice, with no flat claims about glow plugs or carbon and nothing presented as Autoterm's rule.

### 4.1 iOS (`Mobile Apps/ios/Tillerwell`: `ControlView.swift`, `Dials.swift`, Hearth package models)

- **Range control:** minimum 1°C when the box reports `thermo`, falling back to `hyst_min + hyst_max` on old firmware. Remove the hysteresis setting screen when the box reports the new fields.
- **Status line** under the dial when the working range differs from the user's: "Running 19.5–21°C. This space heats quickly, so Hearth widened the range to rest the heater longer." On the controller's sensor: "Running 19–21°C. The controller's sensor reads whole degrees, so Hearth needs a 2°C range." Tap → "Why?".
- **"Why?" screen:** current working range, learned width, the space's recent heating speed ("warms about 0.05°C a minute"), a plain list of changes from `/api/thermo/history` ("12 Nov, 14:20: widened to 1.5°C, 2 of the last 3 runs heated fast enough to climb the range in under 30 minutes" / "15 Nov, 09:10: heating is much slower now, 0.03°C a minute over the last 2 runs, so the range is back to 1°C"), and a **Reset learning** button (manager), with confirmation. Where the working range is wider than the learned width, it says why ("Learned 2.5°C. Running on 4°C, because the controller's sensor reads whole degrees."). When `at_limit` is `"max"`: "This space heats faster than the widest range (4°C) allows. Recent runs, measured on the room sensor, suggest the room may rise above your range before Heater Care can stop the heater." ("measured on the controller's sensor" when that is the one in use.) Nothing is shown for `"min"`.
- **Lock countdowns:** "Heater Care: minimum run, 8 min left" while running ("Heater Care: lighting up" before the flame is established, when there is no countdown yet), "Heater Care: resting, 6 min before the next start" while idle and below the bottom. When a timer has ended: "Timer ended, finishing minimum run: 20 min left".
- **Dips below the range:** when `dip_below > 0`, "Losing heat fast. Dropped to 19.2°C while the heater rested its minimum 20 minutes." The "Why?" screen shows the same per run, and notes runs not counted because of a cold spell.
- **Stop inside the minimum run:** sheet with "Stopping now is a short run. Heaters like this last longer with fewer, longer runs, so Heater Care keeps each one to at least 30 minutes. This one has 8 minutes left." Buttons: **Let it finish** / **Stop anyway**.
- **Thermostat off, or range lowered below the room, inside 30 min:** the same sheet. **Let it finish** makes the change and lets the run reach 30 min. **Stop anyway** stops at once.
- **Manual start inside 20 min of a stop:** the same kind of sheet: "The heater stopped 12 minutes ago. Heater Care rests it for 20 minutes between runs." Buttons: **Wait** / **Start anyway**.
- **Early stop explainer:** when `early_stop_unseen > 0`, a one-time card: "The heater was stopped after 9 minutes (from HomeKit). Heaters like this last longer with fewer, longer runs, so Heater Care's own thermostat always runs at least 30 minutes." Then send `early_stop_seen`.
- **Ceiling and lost readings:** a plain line when either happens: "Stopped early: the room reached 26°C, 5°C over your range." / "No temperature reading. The heater will stop after its minimum run." / "The room sensor has not changed for 15 minutes, so Hearth is using the controller's reading."
- **Timers:** shortest timer 30 min.
- **First time the thermostat is turned on** (stored per phone): a 3-line Heater Care explainer of the 1°C range, learning, and the two locks.

### 4.2 tvOS (`TV/HeaterScreen.swift`)

- Status line and lock countdowns only. No sheets: a stop or thermostat-off from the TV inside 30 min shows a short confirm ("Stop now? It has 8 minutes left of its minimum run."). The explainers stay on the phone. Shortest timer 30 min.

### 4.3 Android (`Mobile Apps/android`)

- The same as iOS, screen for screen.

### 4.4 Device web UI (`components/at_web`)

- Range minimum 1°C, status line, lock countdowns, the stop/start/thermostat-off warnings as a confirm dialog, the "Why?" history as a simple list, Reset learning (manager), shortest timer 30 min.

## 5. Outside the scope chosen, but affected

- **Website heater page (Hearth module, `Cloud Server/cactus-module-hearth/components/public/heater/model.ts`, `lib/signing.ts`):** it uses `hyst_*` for the minimum range. With the compatibility values in §3.3 it keeps working without a change. A follow-up module release could show the working range and the new warnings. That is a module-only change (allowed), released to GitHub only, per the house rules.
- **Matter/HomeKit:** its setpoint and range mapping must allow a 1°C range, and a stop from it is logged as an early stop. No warning is possible there.
- **Home Assistant / MQTT:** the new `thermo` fields go into the state so HA users can see them. A stop from HA is logged as an early stop.

## 6. Docs

- `docs/PROTOCOL.md`, "Start, thermostat mode": the working range, the locks, the ceiling, learning, and how the heater's own burner pause is read.
- `docs/API_SPEC.md`: §3.5 above.
- `docs/TIMERS.md`: the new minimum width, the 30 min shortest timer, and the overrun at a timer's end.
- Tillerwell `CLAUDE.md`: replace the "18–21 acts like the Comfort Controller set to high − 1" paragraph with the new rules (1°C range, learning lowers the bottom, 30/20 min locks, never stopped inside 30 min except by a fault or the ceiling). Done with the firmware change, not after it, so agents do not follow the old rule in between.

## 7. Order of work and release

1. Firmware control loop, locks and ceiling (§3.1), with host tests. First jobs: confirm how the heater's burner pause and "flame established" show in `state_raw`, and what filtering the room reading already has.
2. Learning (§3.2), with host tests.
3. Settings and timers (§3.3, §3.4).
4. Status, history, backup, API_SPEC, PROTOCOL, TIMERS and CLAUDE.md (§3.5, §6).
5. Device web UI (§4.4).
6. Bench test on the test heater (192.168.10.157): run lock, off lock, ceiling, timer overrun, early stop logging, sensor drop, restart during the rest, frost protection's 20 min run. Also run several cycles on each sensor in turn and watch the shared learned width: if the two sensors keep pulling it wider and narrower, give each sensor its own width. **Short taps only, never hold Start.**
7. iOS + tvOS, then Android, each built and installed on the real devices and tested against the bench firmware.
8. **Release everything the same day:** firmware (e.g. 1.47, with clean-up of the build files afterwards) and the apps. The apps also work with the old firmware, so they can be approved and waiting before the firmware goes out.

## 8. Risks

- **Overshoot in small spaces** until learning catches up. With the heater at full output for 30 min, the first few runs can take a small space several degrees over the top. The ceiling limits it to 5°C over.
- **Whole-degree sensor** makes learning coarse, so it may widen one step late.
- **No reading mid-run:** up to a full minimum run with no room reading and no ceiling. Only the heater's own coolant limit applies (decision: accepted, both sensors failing together is rare).
- **Stuck-sensor check** could set aside a good external sensor in a space so large that the reading truly does not move in 15 min of burning. It falls back to the controller's reading, so the cost is a wider range for a while, not a wrong stop.
- **Runs are longer than 30 min** from switch-on, by the light-up time (up to about 15 min by Autoterm's figures).
- **Timers overrun:** a thermostat timer can now end up to 30 min plus light-up late. The status line says why.
- **Old apps on new firmware** show the user range, not the working range, until updated. The thermostat itself behaves correctly.
- **Clock not set at boot:** the saved stop time cannot be used, so the rest counts a full 20 min from boot.

## 9. Later (not part of this build)

- **Warm by a set time:** a timer starts early enough that the room reaches its range at the timer's start time, using the learned heating speed, the typical warm-up delay and the typical cooling time recorded from this build.
