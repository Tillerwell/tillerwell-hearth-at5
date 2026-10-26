# Matter (Wi-Fi; Thread only in its own image)

**Status: the Matter images compile and link; none has run on hardware.** Built against esp-matter `release/v1.5` with ESP-IDF v5.5.5 (esp-matter's README names that pair). What is untested is everything at runtime: pairing, attribute updates, the Thread join (Thread image only). Expect surprises on first power-up.

## Images

| Image | Network | Notes |
|---|---|---|
| `all` | Wi-Fi only, Matter over Wi-Fi, or Matter over Thread, chosen on the device | The recommended image. Default is Wi-Fi only with Matter switched off; the Tillerwell app (`POST /api/mode {"mode":"matter"}`), the web page's Connection mode card or the console `mode matter` restarts into Matter, and the Home app picks Wi-Fi or Thread while pairing (not the user's choice). In Matter mode the box keeps the home Wi-Fi up either way, see below. |
| `matter-wifi` | Matter over Wi-Fi | Smaller, Matter always on. |
| `matter-thread` | Matter over Thread | Smaller. No Wi-Fi at all, so no web page, MQTT or over-the-air upload. |

(An earlier version of this document said one image could not do both Wi-Fi and Thread. That was wrong for esp-matter 1.5, which ships `sdkconfig.defaults.c6_wifi_thread`; `all` uses it.)

In `all`, Wi-Fi mode and Matter mode never run at the same time: the unified firmware decides at boot (a saved setting) which of "our Wi-Fi manager and setup network" or "Matter" owns the radio. Matter's code is linked in but not started in Wi-Fi mode.

### Matter mode keeps the web API (Tillerwell app)

In Matter mode Matter owns the radio. Until the Home app has paired the box and given it Wi-Fi (over Bluetooth), the box is on no network, so the app cannot reach it; that is inherent. Once Matter has joined the home Wi-Fi and the box gets an address, the same start-up as Wi-Fi mode runs: web server (`/api/info`, `/api/status`, `/api/cmd`, `/api/timers`, `/api/settings`, `/api/mode`, ...), SNTP and MQTT. `GET /api/info` reports `mode: "matter"`, `can_switch_mode: true`, `wifi_setup: false` and a `matter` block (`transport` `wifi`, `manual` code, `commissioned`). The web password, if set, still applies.

- **Name on the network:** Matter keeps resetting the shared mDNS host name to its own MAC-derived one, so in Matter mode the firmware publishes `hearth-<id>.local` plus `_http._tcp` / `_hearth-app._tcp` (and `_hearth-ha._tcp` while Home Assistant is switched on) as a *delegated* host (own address record) that Matter cannot overwrite. The address is refreshed on every new IP.
- **Matter over Thread, Wi-Fi kept up (untested):** if the Home app pairs over Thread, `keep_saved_wifi()` in `at_matter.cpp` loads the network saved in Wi-Fi mode into esp-wifi and enables CHIP's own station logic (one Wi-Fi owner, so no second manager to fight CHIP; Wi-Fi, Thread and Bluetooth share the radio through the C6's coexistence). It runs at server start, after commissioning and after a failed-pairing fail-safe. Matter-over-Wi-Fi pairings keep the credentials the Home app handed over. Known gaps: it relies on CHIP's connect/retry behaviour with a configuration it did not write; the Wi-Fi network-commissioning cluster will list the loaded network as already configured, which a controller might read oddly; if you change the Wi-Fi in Wi-Fi mode after a Wi-Fi pairing, Matter keeps its own credentials. None of this has run on hardware.
- **Wi-Fi credentials:** the Wi-Fi name and password saved in Wi-Fi mode are left alone (settings are not touched by the switch). A Wi-Fi pairing uses the credentials the Home app hands over and stores them itself, so the two can differ. `/api/info` `wifi_ssid` is the one saved in Wi-Fi mode. `POST /api/wifi` is refused in Matter mode.
- **Back to Wi-Fi:** `POST /api/mode {"mode":"wifi"}` restarts into Wi-Fi mode, which reconnects with the saved credentials (or raises the setup network if none). The Matter pairing is kept. If nobody pairs within the pairing window, the box falls back to Wi-Fi mode by itself.

## Build

```bash
git clone --depth 1 --branch release/v1.5 https://github.com/espressif/esp-matter.git ~/esp/esp-matter
cd ~/esp/esp-matter && git submodule update --init --depth 1
cd connectedhomeip/connectedhomeip && ./scripts/checkout_submodules.py --shallow --platform esp32 linux
cd ~/esp/esp-matter && ./install.sh --no-host-tool       # see the two Apple-silicon notes below
. ~/esp/esp-idf/export.sh && . ~/esp/esp-matter/export.sh  # IDF must be v5.5.5
./build.sh all build           # or matter-wifi / matter-thread
```

Roughly 5 GB for the checkouts and 1 GB per variant build directory.

Notes from doing this on an Apple-silicon Mac:
- `./install.sh` without `--no-host-tool` also builds chip-tool, which needs a ZAP generator binary that is x86-only here and fails ("Bad CPU type"). The firmware does not need it.
- If the bootstrap's pip step fails building `cffi` with "Bad CPU type in executable: pkg-config", an x86-only `/usr/local/bin/pkg-config` is in the way. Putting a dummy `pkg-config` that exits 1 first on `PATH` for that step worked.

`build.sh` starts from esp-matter's own `examples/light/sdkconfig.defaults` (plus `sdkconfig.defaults.c6_thread` for the Thread image only) so the Bluetooth, mDNS, OpenThread settings are Espressif's; our overlay is applied after. Our overlay turns the clusters we use back on (the light example switches off everything a light does not use, which makes the link fail with undefined `Thermostat` symbols), puts the Wi-Fi soft-AP support back (the light example removes it; the Wi-Fi-only mode's setup network needs it), raises the dynamic endpoint limit (the light example's limit of 2 or 3 is too small for four endpoints) and turns off the CHIP shell so our USB console can have the port.

## What appears in your Matter app

| Endpoint | Device | Meaning |
|---|---|---|
| 1 | Thermostat (heat and cool setpoints, Auto mode) | Current temp = what the thermostat acts on. Auto mode shows the app's range: "heat to" is the bottom (the heater starts below it), "cool to" is the top (it stops above it); the dead band is the app's minimum gap. Mode Auto or Heat = thermostat on, Off = off **and sends Stop**; Cool, Dry and Fan are refused. The heater cannot cool: the cooling setpoint is only the top of the range. |
| 2 | Temperature sensor | Heater coolant temperature |
| 3 | On/off plug "Preheat" | On = preheat for the saved duration, Off = Stop; shows On while the heater runs |
| 4 | Contact sensor "Fault" | Closed = healthy, open = a fault is active |

Matter has no diesel-heater device type, so these are the closest fits. Pump-only, bypass, frost protection and extended telemetry are not exposed over Matter; use the web page (Wi-Fi variants) or the USB console.

## Pairing

Pairing is over Bluetooth; Wi-Fi credentials (or, in the Thread image, the Thread dataset) are handed over by your Matter app. Stay near the box.

The firmware uses esp-matter's **built-in test credentials** (passcode 20202021, discriminator 3840, manual code `3497-011-2332`, vendor ID 0xFFF1). That is normal for hobby devices and works with Home Assistant, Apple Home and Google Home developer pairing, but:

- Apple Home and Google Home warn "uncertified device".
- Anyone within Bluetooth range **while the box is unpaired** can pair it, since the code is public. It becomes unpairable once you pair it. After a factory reset it is open again until you pair, so don't factory-reset it somewhere you don't control.
- Per-device codes need Espressif's factory-data partition (`esp-matter-mfg-tool`); not set up here.

## Thread notes

- Needs a Thread border router on the network.
- The `matter-thread` image has no Wi-Fi, so no web page, MQTT or over-the-air upload. Updates are by USB, or by switching back to a Wi-Fi image the same way.
- Writing down how you flash it before switching is not optional on a boat.

## Home Assistant

HA's Matter integration (Matter Server add-on) can pair either variant. For richer entities (fault names, pump, frost protection, bypass), prefer the `wifi` image with MQTT; HA can also run both: pair Matter for Apple/Google and use `matter-wifi` with MQTT enabled for HA.
