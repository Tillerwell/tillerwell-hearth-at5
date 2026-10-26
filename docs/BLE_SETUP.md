# Bluetooth LE setup (plain Wi-Fi image)

The Tillerwell app sets the box up over Bluetooth: no joining the box's Wi-Fi by hand. The setup access point and web page still work as the fallback ("Set up using Wi-Fi" in the app).

The box advertises as `Hearth-xxxxxx` (same last 3 MAC bytes as the access point) with the service UUID below in the advert. Manufacturer data in the scan response: company `0xFFFF`, one byte, bit 0 = a Wi-Fi network is already saved.

**When it's available:** while the box is not on a Wi-Fi network, for 10 minutes after power-up, and for 3 minutes after the last setup write. Otherwise it stops advertising and refuses setup writes, so a working box can't be re-pointed by a passer-by. Power-cycling the box opens the window again. There is no pairing or encryption: the Wi-Fi password crosses the air in clear over a short-range link, like the open setup access point does.

Service `6e2f0001-5d1c-4f3a-9b7e-2a4c8d1e0f01`. Characteristics share the base with the 16-bit part changed:

| UUID | Props | Value |
|---|---|---|
| `6e2f0002-…` Info | read | `{"device_name":"hearth-a1b2c3","configured":false,"open":true,"ip":"","wifi_ssid":"","proto":1}` |
| `6e2f0003-…` Scan | write, read | write anything to start a Wi-Fi scan; when Status `scan` becomes `done`, read `[{"ssid":..,"rssi":..,"auth":..},..]` (at most ~512 bytes, strongest first) |
| `6e2f0004-…` Provision | write | `{"ssid":"…","pass":"…"}`; the box saves it and joins |
| `6e2f0005-…` Status | read, notify | `{"state":"idle\|connecting\|connected\|failed","ip":"192.168.1.20","reason":"auth\|not_found\|other","scan":"idle\|busy\|done"}`. `reason` only with `failed` (after 3 failed attempts). Notifications can be cut to the MTU: read the characteristic when one arrives. |

`device_name` is also the mDNS hostname (`hearth-a1b2c3.local`) and the ID the app keys on. The friendly name ("Cabin heater") lives only in the app.
