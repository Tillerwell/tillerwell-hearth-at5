# Switching a run between preheat and thermostat (firmware 1.68)

A run that is already going can be switched between preheat and thermostat without the burner stopping. A stop
followed by a start would cost a purge, a 20 minute rest and a fresh ignition, which is exactly the short cycling
Heater Care is there to prevent.

## The command

`{"cmd":"run_mode","mode":"thermostat"}` or `{"cmd":"run_mode","mode":"preheat","minutes":45}` (permission:
control). `minutes` is optional and only used for preheat.

Accepted only while the heater is starting or burning (`state_raw` 2 or 3) and no stop is under way. Otherwise it is
refused: locally `400 {"ok":false,"error":"The heater is not running, ...","care":"not_running","left_s":0}`, through
the cloud an ack `rejected` with reason `heater_care_not_running`. An older box does not know the command: `400`
locally, `refused` through the cloud.

`run_mode` in `/api/status` says what the run is now (`thermostat`, `preheat`, or null before any start), and from
1.68 `preheat_total_min` is the preheat's run time in minutes counted from its start (null for a thermostat run or
when not known), so every app can show when it will shut down: time left = `preheat_total_min - elapsed_min`.

## Preheat to thermostat

1. Auto goes on (if it was not) and the run becomes Auto's: it is stopped at the top of the range, with every Heater
   Care rule (30 min minimum run from flame, the ceiling at the top + 5 C, the learned early stop).
2. The heater still holds the preheat's run time and would stop itself when it runs out, so the box gives it a long
   run time with `type02`: the run so far plus 600 minutes, topped up an hour before it would run out, for as long as
   the thermostat owns the run. The heater ignores a start (`type01 ff fb`) while it is burning (tested on the bus,
   [PROTOCOL.md](PROTOCOL.md) "Switching a run"); 1.68 sent one, so on 1.68 a switched run still stopped when the old
   preheat time ran out. Never `00 22` (CLAUDE.md, PROTOCOL.md).
3. `run_mode` becomes `thermostat`, `hearth_run` becomes true (the Comfort Controller's own preheat stop is held
   back, as for any Hearth run).

The same happens whenever Auto is switched on while a preheat is running, from any surface (an app, the website,
Home Assistant's Heat, Matter, MQTT): before 1.68 Auto took the run over but the heater still stopped itself when
the preheat time ran out.

The run is not learned from (it did not start as a thermostat run).

## Thermostat to preheat

1. The box sends the heater a run time with `type02`: the minutes asked for (or the saved `preheat_minutes`) from
   now, never less than what is left of the 30 minute minimum run, so the heater's own stop cannot short-cycle it.
   The frame carries the total counted from the run's start, which is what the heater counts
   ([PROTOCOL.md](PROTOCOL.md), "Switching a run").
2. Then Auto goes off **without a stop**. The run is nobody's, as a preheat is: Hearth does not stop it at the top or
   at the ceiling; the heater stops itself when its time is up (or earlier on its own coolant limit).
3. `run_mode` becomes `preheat`; Auto off is saved, so the thermostat does not start the heater again afterwards.

**Backstop.** The box keeps its own watch as well: if a run
switched to preheat is still burning 5 minutes after the time it was given (counted from the run's start, as sent),
Hearth stops it, logged as the heater's own end (`heater`). A later `preheat_duration` moves the backstop with it, and
switching back to thermostat clears it. So a heater that ignored the frame, or counts from the frame rather than the
start, still stops about when it should; it can never carry on indefinitely with the thermostat off.

If the `type02` cannot be queued (Bypass, a full queue) nothing changes and the command fails: Auto is never switched
off on a run that would then carry on indefinitely with nobody to stop it.

A thermostat timer that owns the run ends ("Auto switched off") and puts the user's range back, as when Auto is
switched off by hand. The run is not learned from.

## Heater Care

A switch is neither a stop nor a start, so the minimum run and the rest do not refuse it, from any caller. The minimum
run is still kept: preheat time is never shorter than what is left of it, and a thermostat run is never stopped
inside it. There is no override path, and none is needed.

## While the heater cools down

After a thermostat run (state `cooldown`, Auto still on) the set temperature can be changed and the thermostat can be
switched off, from every surface. From 1.68 Auto off while the heater is idle or already cooling down sends no stop
frame (there is nothing to stop); it only disarms Auto, so the heater does not start again when the room cools. An
explicit `stop` during the purge is still accepted (the Heater Care lock never applies once the heater is cooling
down) and is harmless: the Comfort Controller itself sends `type03` repeatedly through the purge.

## Surfaces

- iOS and tvOS, Android: while running, a Preheat / Thermostat switch on the control screen (needs 1.68).
- Website (Hearth module): the same switch on the heater page.
- Home Assistant: climate `preset_mode` `preheat` / `thermostat` on `climate.hearth_*`, while the heater is running.
