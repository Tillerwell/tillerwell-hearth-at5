# Tillerwell Hearth: inline bridge for Autoterm heaters (ESP32-C6)

*Hearth is a Tillerwell product. It works with Autoterm heaters and is not made by or affiliated with Autoterm.*

Firmware for a small ESP32-C6 box that sits **between an Autoterm heater and its comfort controller**. It passes every frame through unchanged, so the controller keeps working, and it adds what the controller lacks: Wi-Fi / Matter control, a web page, Home Assistant entities, a software thermostat and frost protection.

Based on [sinise/autoterm-heater-control](https://github.com/sinise/autoterm-heater-control) (MIT), a Home Assistant add-on that does the same job from a Raspberry Pi or a server with two USB-serial cables. The protocol knowledge, the divert-and-replay command injection and the thermostat rules are ported from there; `host_tests/` checks the C code byte-for-byte against that project's Python. The protocol is in [docs/PROTOCOL.md](docs/PROTOCOL.md). It is confirmed on a real Flow 5 / BINAR-5S by the original author, **not yet on this hardware** (see "Before you trust it").

## Firmware images

**`all` is the one to flash.** It starts in Wi-Fi mode and you pick the connection on the device, no re-flashing:

| Mode | Network | Set up by | Controls |
|---|---|---|---|
| **Wi-Fi only** (default) | Wi-Fi | Phone joins the device's own "Hearth-<id>" network, a captive page takes your Wi-Fi password | Web page, MQTT + Home Assistant discovery, USB console |
| **Matter over Thread** | Thread (+ saved Wi-Fi kept up) | Matter app over Bluetooth, needs a Thread border router | Matter, plus the web page and MQTT over the kept Wi-Fi (untested) |
| **Matter over Wi-Fi** | Wi-Fi | Matter app over Bluetooth (Wi-Fi password travels in the pairing) | Matter, plus the web page and MQTT |

Wi-Fi only vs Matter is a switch on the web page (Setup → Connection mode) or `mode wifi` / `mode matter` on the USB console; it restarts the device. Within Matter, your Home app chooses Wi-Fi or Thread while pairing; either way the box keeps the home Wi-Fi up in Matter mode, so the Tillerwell app keeps working over the same web API at `hearth-<id>.local`. Switching modes keeps the Matter pairing, so going back to Matter later needs no re-pairing. The BOOT button no longer enters Matter mode: units are always paired over Wi-Fi. Hold it 5 s (the LED flashes slow yellow) and let go and nothing happens; keep holding to 10 s (the LED flashes fast red) and let go for a factory reset, which erases everything, pairing included, and returns to Wi-Fi mode with the setup network. Keep holding past 15 s and the LED goes back to normal and letting go does nothing, which is how to call off a reset. LED in Wi-Fi mode: slow flashing blue = not set up yet (fast while a phone is connected over Bluetooth), solid green = on the network, fast flashing yellow at full brightness = Wi-Fi dropped. If nobody pairs it within 15 minutes of entering Matter mode, it goes back to Wi-Fi mode by itself.

There is only one firmware image (`./build.sh build`, `build/all/hearth.bin`); the modes above are chosen on the device.

**Which mode:** start with Wi-Fi only. Choose Matter if you want Apple Home, Google Home, Alexa or Home Assistant's Matter integration without MQTT; Wi-Fi only gives the richest Home Assistant entities (fault names, pump-only, frost protection, bypass). Thread is only worth it if you already have a Thread border router (HomePod mini, Apple TV 4K, Nest Hub 2nd gen, Home Assistant with an OTBR). See [docs/MATTER.md](docs/MATTER.md).

## Hardware

ESP32-C6 (Waveshare ESP32-C6-Zero or a C6 Supermini, 8 MB flash). The build diagram is `Hardware/Hearth/Wiring Diagrams/wiring-diagram-v3-esp32-standalone.html` in the Tillerwell folder; pins:

| Pin | Function | Wire |
|---|---|---|
| GPIO0 | UART1 RX | heater green (heater TX), through a divider: 1 kΩ in series, 2 kΩ to ground |
| GPIO1 | UART1 TX | heater white (heater RX) |
| GPIO18 | UART0 RX | controller white (controller TX), through 1 kΩ in series only, **no divider** |
| GPIO19 | UART0 TX | controller yellow (controller RX) |
| GPIO9 | BOOT button | release after 10 s: factory reset; before 10 s or after 15 s: nothing |

Both RX pins have the internal pull-up switched on (`uart_setup()` in `components/at_app/at_app.c`). The controller side depends on it: the controller's data output cannot hold its line up against a 2 kΩ divider, so with a divider GPIO18 sits at 0 V and nothing gets through, and with no divider it needs the pull-up to idle high. Keep the pull-up in any firmware for this hardware.

The ESP32-C6 has only two general UARTs, so the controller side uses UART0 and the log/console moved to the USB port (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`). Pins and baud rate are in `idf.py menuconfig` → *Autoterm Flow 5D bridge*.

## Setup, plain Wi-Fi image

1. Flash it (see [docs/BUILDING.md](docs/BUILDING.md)) and power the box.
2. On a phone, join the Wi-Fi network **Hearth-<id>**. The setup page opens by itself (or go to `http://192.168.4.1`).
3. Setup tab → *Scan networks*, tap yours, enter the password, *Save and connect*.
4. Rejoin your normal Wi-Fi and open `http://hearth-<id>.local` (the name is on the Setup tab and in the router's client list).
5. Optional: add it to Home Assistant (below) and set a web password.

If the saved network can't be reached for two minutes the setup network comes back, so a changed router password never strands the box. No USB needed; with USB you can also type `wifi <ssid> <password>` in the console.

## Home Assistant

Install the Tillerwell integration in Home Assistant (HACS). The box announces itself on the network as `_hearth-ha._tcp` all the time, so Home Assistant finds it on its own (Settings → Devices & services), or you can add it by address. There is nothing to switch on in the box's page. The integration talks to the box over HTTP and does not use MQTT.

## Setup, Matter images

1. Flash, power, bring the phone close (pairing is over Bluetooth).
2. In the Matter app choose *Add device* and enter the manual pairing code printed in the USB log and shown by the `matter` console command (default test code `3497-011-2332`, see the security note in [docs/MATTER.md](docs/MATTER.md)).
3. The device appears as a thermostat (target, mode off/heat, current temperature), a coolant temperature sensor, a "Preheat" switch and a "Fault" contact sensor.

Factory reset (settings and Matter pairing): hold BOOT 10 s and let go, or `factory-reset yes` in the console, or the button on the web page.

## What it does

- **Standalone**: if the controller is silent for 5 s (unplugged, or its line broken), the box polls the heater itself: a status query once a second and, when a room temperature is known (MQTT external temperature, else the controller's last reading), a cabin temperature report every 10 s. Own polls show as `O` in the bus log and `standalone` in `/api/status`. It stops as soon as the controller speaks again.
- **Relay**: both directions, frame by frame. The ESP32 restarting briefly cuts the controller off from the heater; there is no bypass relay in this hardware.
- **Commands**: start (thermostat mode), preheat N minutes, pump only, stop. Sent as the controller, using the divert-and-replay trick: the controller is answered from cached replies for about a second while the command goes to the heater.
- **Auto thermostat**: stop at target + 1 °C, start at target − 1 °C. Uses the controller's own temperature (whole °C), or a better room temperature you publish to MQTT `hearth/<name>/cmd/external_temp/set` (used while fresher than 90 s).
- **Timers**: up to 32 scheduled runs (repeating on chosen days, or one-off), kept on the box so they fire with the phone away. Needs real time (network time, or set by the app) and a time zone. See [docs/TIMERS.md](docs/TIMERS.md).
- **Prevent freezing**: starts the heater at the frost floor even if Auto is off or you pressed Stop; only stops runs it started.
- **Faults**: never switch either loop off; retries back off 5, 15, 20 minutes, then wait for the fault to clear.
- **Bypass switch**: relay only, never sends anything.
- **Extended telemetry** (experimental, off): the vendor tool's handshake once a minute for supply voltage, flame temperature and so on. Flow 5 / BINAR-5S layout only. Can upset the controller on some units.
- **Bus log** on the Diagnostics tab and counters for CRC errors, so a wrong wire colour shows up immediately.
- **Line probe** for wiring checks: `/api/status` reports, for each RX pin over the last second, the share of time it was high (`rx_heater_high_pct`, `rx_controller_high_pct`) and how often it changed (`rx_heater_edges`, `rx_controller_edges`). An idle line reads about 100 % high; 0 % with no edges means the line is held low (a divider, or a wire that is not the data line); 100 % with no edges for long means nothing is sending on it.

## Before you trust it

First run on a real Autoterm Flow 5D with its Comfort Controller, 2026-10-26: relaying works both ways at 2400 baud with no CRC errors, the heater answers the box's own polls in standalone mode, and the controller shows the heater normally through the box. **Commands sent by the box (start, stop, preheat, pump) have not been tried on the heater yet.** Things to check on a new build, in this order:

1. Idle voltage of the heater's green wire (5 V → divider on GPIO0; 3.3 V → none). The controller side gets 1 kΩ in series only, whatever it measures.
2. Flash, then with the heater and controller connected, check `/api/status`: `frames_heater` and `frames_controller` must both count up and the `bad_crc_` counters stay at 0. If a side is silent, read the line probe (`rx_..._high_pct`, `rx_..._edges`) before swapping wires. With the controller unplugged, `standalone` should turn true and `frames_heater` keep counting.
3. Turn **Bypass on** and watch the controller operate normally through the box for a while.
4. Only then turn Bypass off and try *Stop*, then *Start*, with someone at the heater.
5. If the heater ignores commands though it is heard, the ESP32's 3.3 V TX into a 5 V input may be the problem (planning notes: 74AHCT125 buffer).

## Versions

The firmware is RoamOS, numbered for Hearth on its own (brand guidelines, Naming): one digit, a point and four digits, shown as *Hearth · RoamOS 0.0001*. Set it in one place, `ROAMOS_VERSION` in `CMakeLists.txt`, and add one to the four digits for every release. The build stops on any other format (`cmake -P roamos_version.cmake` runs the checks).

- The web page, `/api/info` (`version`) and the status JSON (`os_name`, `os_version`) show the string, e.g. `0.0001`.
- Matter's software version is the same number as a whole number (0.0001 is 1, 0.1234 is 1234, 1.0002 is 10002), and its version string is the same text. Matter's OTA requestor only takes an update with a higher number, so never reuse or lower a version.
- Uploading a `.bin` on the web page (`POST /api/ota`) reads the image's version before erasing anything. It refuses an older RoamOS version, or a version not in RoamOS form, with 409 and a reason. To roll back, tick *Allow an older version* on the page, which sends `?allow_older=1`. The same version again is always allowed. The rules are in `components/at_core/at_version.c` and host-tested.

## Layout

```
components/at_proto    CRC, framer, status decoding        (pure C, host-tested)
components/at_core     relay + injector state machine, thermostat, timers, time zones  (pure C, host-tested)
components/at_app      settings (NVS), UART tasks, shared state
components/at_net      Wi-Fi, setup AP, captive DNS
components/at_web      web UI + API + OTA
components/at_mqtt     MQTT client + Home Assistant discovery + built-in broker (managed component espressif/mosquitto)
components/at_matter   Matter endpoints
main/                  startup, USB console, factory-reset button
host_tests/            make -C host_tests
```

Licence: MIT, as the base project (see LICENSE).
