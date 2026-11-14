# The Heater Care lock (firmware 1.61, RoamOS 1.0061)

Heater Care's two rules protect the heater from short cycling:

- **Minimum run.** A run is not stopped until it has burned 30 minutes from flame established (20 for a frost
  protection run). Lighting up does not count towards it.
- **Rest.** After a run, the heater is not started again until it has been idle for 20 minutes.

Hearth's own thermostat has always kept to both. Before 1.61, anything else could break them: the iOS Home app or
Google Home over Matter, Home Assistant, MQTT, the website or the box's own page could stop a run a few minutes in, or
start the heater a few minutes after it went idle. From 1.61 the box refuses those requests.

## Who may override

Only the Tillerwell apps (iOS, Apple TV, Android). They explain what an early stop or a start inside the rest costs,
and offer "Stop anyway" / "Start anyway". When the user picks one of those, the app sends the command with
`"override": true`.

The box honours the override only on a **signed** request:

| Path | Signed | May override |
|---|---|---|
| `POST /api/cmd` with a `Hearth-Auth` header (the apps on local Wi-Fi) | yes | yes |
| Cloud command (§9.1 of API_SPEC; the apps away from home) | yes | yes |
| Website's account route for the Apple TV app (`/api/m/hearth/public/app/devices/<id>/control`) | yes, with the website's key | yes: only this route passes `override` on |
| Website's own control page | yes, with the website's key | no: the page never sends it, and its route drops it |
| `POST /api/cmd` without a signature (Home Assistant, the box's own page) | no | no: `override` is ignored |
| Matter (Apple Home, Google Home, any Matter controller) | n/a | never |
| MQTT (`at_mqtt`, Home Assistant MQTT) | n/a | never |
| Box timers | n/a | never |

`"finish_run": false` (the apps' "Stop anyway" for thermostat off and for a set temperature below the room) is also
an override: from a caller that may not override it is read as `true`, so the run finishes its minimum run first.

## What is locked

`at_ctl_care_stop` / `at_ctl_care_start` in `components/at_core/at_ctl.c`, applied by `at_app_stop_care`,
`at_app_start_preheat_care` and `at_app_start_thermostat_care` (`components/at_app/at_app.c`). The plain
`at_app_stop_from`, `at_app_start_preheat` and `at_app_start_thermostat` are the locked versions, with no override.

- **Stop** is refused while the run has part of its minimum run left (`at_ctl_min_run_left_ms`), unless the heater
  shows a fault or is no longer starting or burning (already cooling down: a stop then does nothing anyway).
- **Start** (preheat or thermostat start) is refused while the rest has time left (`at_ctl_min_off_left_ms`). A start
  while a run is still going (for example during the purge after a stop) is not locked: it carries the same run on.
- **Thermostat off** (Matter system mode Off, Home Assistant's climate off, `auto` `enabled:false`) is accepted. Inside
  the minimum run, Auto goes off and the run finishes its minimum run, then Hearth stops it. Nothing is refused.
- **Thermostat on** inside the rest is accepted. The first start waits for the rest to end, as it always has. With the app's override (`auto` `enabled:true` `override:true`, its "Start anyway") the rest is cleared and the thermostat may start at once.
- Faults, the ceiling (top + 5 °C) and Hearth's own stops are not affected.

## What a refusal looks like

- **Matter:** the write to the preheat On/Off attribute fails (`ESP_ERR_INVALID_STATE`). Apple Home and Google Home
  put the switch back. Matter has no way to show the reason.
- **Local HTTP:** `400` with
  `{"ok":false,"error":"Heater Care: the run has 17 more min of its minimum run to go before it can stop. Only the Tillerwell app can stop it sooner.","care":"min_run","left_s":1013}`
  (`care` is `min_run` or `min_off`). Home Assistant shows `error` as the reason (integration 1.13.0). A HomeKit
  switch bridged from Home Assistant flips back.
- **Cloud:** the ack is `rejected` with reason `heater_care_min_run` or `heater_care_min_off`.
- **Box timers:** a preheat timer that would start inside the rest is skipped ("skipped: command not sent").
  Thermostat timers turn Auto on, which waits for the rest as above.
- Every refusal and every override is logged (`app` tag).
