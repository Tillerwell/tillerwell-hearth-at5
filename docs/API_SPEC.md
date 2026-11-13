# Hearth API and signing spec, version 1

> Status: **agreed 2026-10-28** (phase L1 of `Cloud Server/CLOUD-PLANNING.md` in the Tillerwell planning folder). Phase L2 (firmware 1.0027) enforces §4 to §7 on **plain HTTP port 80** (see §3 and §11 item 7); HTTPS is still to come. Phase C4 (firmware 1.0030) adds the cloud client of §9. Until an admin switches it off, the box also still serves the old unauthenticated API described in `Mobile Apps/ios/Tillerwell/DEVICE_API.md`.
>
> This file is the single source of truth for the box, the iOS and Android apps, the website's `hearth` module and the Hearth worker. Every rule that produces bytes is pinned by `docs/auth-vectors.json`; `make -C host_tests auth` checks the firmware against them. If this text and the vectors disagree, the vectors are right and this text is the bug.

## 1. Goals

- A box is fully usable with no account and no internet (cloud plan §1). Every trust decision is made **on the box**.
- One user key works locally and through the cloud. The cloud carries signed messages but can never authorise anything itself (cloud plan §3.1).
- Simple enough to implement identically in C (mbedTLS), Swift (CryptoKit) and Kotlin (Android Keystore / java.security), with test vectors for each.

## 2. Building blocks

| Thing | Format |
|---|---|
| Keys | ECDSA on NIST P-256 (secp256r1). Public key on the wire: SEC1 **uncompressed** point, 65 bytes, `0x04 ‖ X ‖ Y` |
| Signature | ECDSA with SHA-256 over the exact bytes given below. Encoded as **raw `r ‖ s`, 64 bytes**, each big-endian and left-padded to 32 bytes (not DER: CryptoKit's `rawRepresentation`; Android must convert from DER). Signers should use deterministic nonces (RFC 6979) so the vectors reproduce byte for byte; verifiers accept any valid signature |
| base64url | RFC 4648 §5 alphabet (`-` and `_`), **no padding**. Decoders reject padding, the standard alphabet and non-zero leftover bits |
| Key id (`kid`) | base64url of the first 8 bytes of SHA-256(65-byte public key): 11 characters |
| Device id | `hearth-` plus the box's factory MAC address as 12 lowercase hex digits, e.g. `hearth-a0b1c2d3e4f5`. Globally unique and never changes. (The friendly mDNS name `hearth-a1b2c3` stays as it is, but it uses only 3 MAC bytes, so it is not an identity) |
| Time | Unix seconds, integers |
| JSON | UTF-8. A signature always covers the **exact bytes sent**, never a re-serialisation, so no canonical form is needed |

Every signed message starts with a fixed domain string (`HEARTH-REQ-1`, `HEARTH-GRANT-1`, `HEARTH-CMD-1`), so a signature made for one purpose can never be replayed as another.

## 3. Transport

- The signed API is **HTTPS on port 443**. At first setup the box makes its own P-256 key and a self-signed certificate (10 years, CN = device id) and never changes it short of a factory reset.
- **Pinning:** the box's *fingerprint* is the base64url SHA-256 of its certificate's DER `SubjectPublicKeyInfo`. The phone records it at claim time; invited users get it in the invite (§7). Apps must refuse a box whose fingerprint differs, whatever the certificate says.
- **Decided for 1.0027:** the heap test has not been run on a bench box, so the signed API is served on **plain HTTP port 80**, the fallback below, and `fingerprint` in `GET /api/hello` is `""` (`"transport":"http"`). HTTPS moves to a later release once the test passes; signed requests do not change when it does.
- **To be proved on hardware first** (cloud plan Q10): a TLS server's heap use beside Matter, Wi-Fi and Bluetooth on the ESP32-C6. If it does not fit, the fallback is the same signed API over plain HTTP on port 80. The signatures already stop forgery and replay; what is lost is privacy of the readings on the local network. That decision gets recorded here.
- The old web page and its plain-HTTP API on port 80 stay as a **separate** trust path with their own optional password, which the admin can switch off (cloud plan §3.1). They never accept a signed-API key, and the signed API never accepts that password. On the shared port a request is signed if it carries a `Hearth-Auth` header (then it is checked, and a bad one is refused, never retried as the old API) and old otherwise. `POST /api/auth/legacy` `{"enabled":false}` (admin, signed) switches the old API off: unsigned requests then get `401 {"error":"auth_required"}`; the page itself is still served. It cannot be switched off while the box has no admin, and it switches itself back on if the last admin is ever removed.

## 4. Users

The box keeps a user table in NVS, **at most 16 entries**:

| Field | |
|---|---|
| `kid` | key id |
| `pub` | 65-byte public key |
| `perm` | `view`, `control`, `manager` or `admin` |
| `label` | up to 32 bytes of UTF-8, shown in the app |
| `exp` | grant expiry (0 = none). Best effort: only enforced while the box has trustworthy time |
| `by` | `kid` of the admin who added it |

Plus a list of the last 32 redeemed grant ids (`gid`), so an invite can be used once.

## 5. Signed local requests

### 5.1 Getting a nonce

`GET /api/auth/nonce` → `{"nonce":"<base64url of 16 random bytes>","ttl":60}`

- No authentication. The box keeps at most 16 outstanding nonces, oldest dropped first.
- A nonce is good for **one** request and for 60 seconds. Using it, successfully or not, spends it.
- Challenge-response rather than timestamps because a box with no internet has no reliable clock (cloud plan §3.1).

### 5.2 Signing a request

String to sign: these six lines joined with `\n` (0x0A), no trailing newline:

```
HEARTH-REQ-1
<device id>
<method, upper case>
<path exactly as sent, including any query string>
<nonce>
<lowercase hex SHA-256 of the body bytes; of the empty string when there is no body>
```

Header:

```
Hearth-Auth: v1 kid=<kid> nonce=<nonce> sig=<base64url signature>
```

The device id is in the signed string so a request signed for one box cannot be replayed on another.

For `POST /api/ota` the body is the whole firmware image, too big to hold, so the box hashes it as it arrives and checks the signature **after the last byte, before it boots the new image**; a bad signature leaves the running firmware alone.

### 5.3 What the box does

1. Parse `Hearth-Auth`. Missing or malformed: `401 {"error":"auth_required"}`.
2. Spend the nonce. Unknown or expired: `401 {"error":"nonce_invalid"}` (get a new one and retry once).
3. Look up `kid`. Not in the table: `401 {"error":"unknown_key"}`.
4. Rebuild the string to sign from the request **as received** and verify. Failure: `401 {"error":"bad_signature"}`.
5. Check the permission (§6). Too low: `403 {"error":"forbidden","need":"control"}`.

Every authenticated response carries `Hearth-Nonce: <fresh nonce>`, so a busy app needs only one extra round trip per session.

## 6. Permissions

Levels are ordered: `view` < `control` < `manager` < `admin` (cloud plan Q8).

| Route | Needs |
|---|---|
| `GET /api/auth/nonce`, `GET /api/hello`, `POST`/`GET /api/auth/claim`, `POST /api/auth/redeem` | nothing (each checks what it needs itself) |
| `GET /api/status`, `/api/info`, `/api/timers`, `/api/fuel`, `/api/thermo/history`, `/api/auth/me` | view |
| `PUT /api/timers` | control |
| `POST /api/cmd` | depends on `cmd`, below (at least control) |
| `GET /api/wifi/scan`, `POST /api/wifi`, `POST /api/mode` | manager |
| `/api/auth/users…`, `/api/cloud…`, `/api/backup`, `POST /api/ota`, `POST /api/auth/grant-revoke` | admin |
| anything else | admin (fail closed) |

`POST /api/cmd`:

| Needs | Commands |
|---|---|
| control | `start_preheat`, `stop`, `auto`, `auto_target`, `auto_range`, `preheat_duration`, `set_time`, `tank_level`, `tank_filled`, `heater_read`, `start_pump`, `fuel_prime`, `timer_set`, `timer_delete`, `early_stop_seen` |
| manager | `heater_unlock`, `heater_settings`, `fan_test`, `pump_minutes`, `hysteresis` (ignored since 1.47, §9.10), `thermo_learn_reset`, `preheat_temp`, `preheat_minutes`, `timezone`, `display_name`, `fuel_price`, `fuel_factor`, `fuel_rates`, `tank_capacity` |
| admin | `factory_reset`, `reboot`, `auto_update`, `ota` (cloud only, §9.7), and **any command not listed** (fail closed) |

`GET /api/hello` (public) tells a phone what it is talking to before it holds a key: `{"device_id","name","version","claimed":true|false,"fingerprint"}`.

## 7. Claiming and invitations

### 7.1 Claiming the first admin (cloud plan Q12)

Only possible while the box has **no admin**.

1. The app makes a key (iOS: Keychain, synchronisable so it rides the iCloud backup; Android: encrypted backup; cloud plan §3.1).
2. `POST /api/auth/claim` with body `{"pubkey":"<base64url 65 bytes>","label":"Chris's iPhone"}`, signed as in §5 with **that** key (proof that the app holds it). From firmware 1.28 the box makes the key the admin at once and answers `202 {"state":"claimed"}`; no button is needed. (Firmware before 1.28 answered `202 {"state":"press_button","window_s":60}` and waited for a short press of the box's button; `GET /api/auth/claim` → `{"state":"waiting"|"claimed"|"timeout"}` reports that wait, and the app still handles it.)

The first key to ask while the box has no admin wins; a box that already has an admin answers `409 already_claimed`.

The app records the fingerprint (§3) at this point.

### 7.2 Invitations (grants)

An admin invites someone **in person** with a QR code. The admin's app makes a fresh key pair for the guest and signs a grant for its public key, so nothing needs to be registered anywhere.

Grant payload (JSON; the exact bytes are what is signed):

```json
{"v":1,"typ":"grant","gid":"<base64url 12 random bytes>","dev":"<device id>",
 "key":"<guest public key, base64url>","label":"Sam's phone","perm":"control",
 "iat":<now>,"redeem_by":<now + 900>,"exp":<0 or an expiry>,"by":"<admin kid>"}
```

- Signed bytes: `HEARTH-GRANT-1` ‖ `\n` ‖ payload.
- **Token:** `<base64url payload>.<base64url signature>`.
- **QR text:** `hearth-invite:1:<device id>:<fingerprint>:<token>:<guest private key, base64url of the 32-byte scalar>`.
- `perm` may be `view`, `control`, `manager` or `admin` (an extra admin is how the admin makes a backup phone, §3.1 of the cloud plan).

### 7.3 Redeeming

The guest's app stores the private key from the QR, then sends `POST /api/auth/redeem` with body `{"grant":"<token>"}`, signed as in §5 with the **guest's** key. The box:

1. reads `by` from the payload and finds that admin (not an admin in its table: `401 {"error":"unknown_signer"}`);
2. verifies the grant signature with the admin's key (`bad_signature`);
3. checks `dev` is itself, `key` is the key that signed this request (`wrong_key`), and `gid` has not been redeemed (`already_used`);
4. checks `redeem_by` if it has trustworthy time (best effort, as `exp`; the one-time `gid` is what really stops re-use);
5. adds the user and records `gid`.

### 7.4 Removing users

`GET /api/auth/users` (admin) lists the table as `{"users":[{"kid","pub","perm","label","exp","by"}]}`. `DELETE /api/auth/users/<kid>` (admin) removes one; the box refuses to remove the last admin. `POST /api/auth/grant-revoke` `{"gid":"…"}` (admin) burns an invite that has not been used yet. The cloud is told separately (§9.4), so it can drop that key's traffic early.

`GET /api/auth/me` (any signed user) answers `{"kid","label","perm","exp"}`. A user whose `exp` has passed is refused as `unknown_key`, but only while the box has trustworthy time (it has had network time, or the phone set the clock).

Redeem errors: `unknown_signer`/`bad_signature` 401, `wrong_device`/`wrong_key` 403, `already_used`/`table_full` 409, `expired` 410, `malformed` 400.

### 7.5 Replacing a heater (firmware 1.41)

When an owner's box dies, the app sets up the new box and the cloud moves the old box's record onto it (the old certificate is revoked). The app then restores two things over the local API, both admin only:

- `POST /api/auth/users` `{"users":[{"pub":"<base64url 65 bytes>","perm":"view|control|manager|admin","label":"…","exp":0}]}` adds users by public key with no invite: the old box's other admins, its invited users and the website's key (§9.6). The signing admin's own key is skipped, so it is never downgraded. Answers `{"ok":true,"added":n,"skipped":n}` (skipped = malformed entries), or `409 {"error":"table_full"}` at the first one with no room (those before it stay added).
- `GET /api/backup` returns the owner's settings, `PUT /api/backup` applies them:

```json
{"v":1,"display_name":"Saloon","auto_low":20,"auto_high":21,"auto_target":20.5,"hyst_min":0.5,"hyst_max":0.5,
 "use_external_temp":true,"preheat_temp":88,"preheat_minutes":30,"freeze_enabled":false,"freeze_target":5,
 "timezone":"Europe/London","auto_update":false,"fuel_price":1.45,"fuel_factor":1,
 "fuel_rates":{"starting":0.3,"low":0.12,"middle":0.3,"high":0.6},"ml_per_pulse":0,"tank_capacity":0,"timers":[…],
 "thermo_learn":{"width":1.5,"last_change":"widened","window_ext":[{"need":1.7,"rate":0.05,"vote":"fast"}],"window_ctl":[],
  "warmup_ext":[360,420],"warmup_ctl":[],"cooling_ext":[2700],"cooling_ctl":[],"limit_ext":null,"limit_ctl":null,"history":[…]}}
```

`thermo_learn` (firmware 1.47, §9.10) is what Heater Care learned about the space, so a replacement box restored from the cloud carries on from it. It is optional and older boxes ignore it; `v` stays 1. `hyst_min`/`hyst_max` in a backup are ignored since 1.47.

`heater_care` (firmware 1.54) is the `GET /api/thermo/history` document (§9.10) without `early_stops`: Heater Care's changes and its last runs. The website keeps it so apps away from the box can show the Heater Care screen. `PUT` ignores it: the runs belong to the old box.

`timers` is the `GET /api/timers` array. Nothing in it belongs to one box alone (Wi-Fi, users, keys, the fuel log) or to the moment (whether the heater is running, whether the thermostat is on), so restoring never starts or stops the heater. `PUT` applies each field through the same checks as `POST /api/cmd`; a field the box turns down is named in `{"ok":true,"skipped":[…]}` and the rest still apply. `ml_per_pulse` and `tank_capacity` of 0 mean "never set" and are left alone. A changed `display_name` restarts the box, so it is applied last.

The box also sends this document to the cloud (§9.3), which keeps the last copy so it is there when the box is not.

## 8. Cloud enrolment (opt-in)

All admin-only, over the signed local API:

1. `POST /api/cloud/csr` → the box makes (once) a separate P-256 key for the cloud and returns `{"device_id","csr_pem"}`, subject exactly `CN=<device id>`.
2. The app sends the CSR to the website (`POST /api/m/hearth/public/enrol`, signed in as the admin's account). The Hearth worker signs it with the issuing CA and the app collects the certificate chain.
3. `POST /api/cloud/cert` `{"cert_chain_pem":"…"}` → the box checks the chain ends at the **pinned Tillerwell Hearth Root CA**, the leaf is `CN=<device id>` and matches its key, then starts the cloud MQTT client.
4. `DELETE /api/cloud` → forget the certificate and stop the cloud client.

## 9. Cloud messages (MQTT, broker `mqtt.tillerwell.com:8883`, mutual TLS)

Topics are `hearth/<device id>/…` as in cloud plan §3.

### 9.1 Commands (`cmd`, cloud to box)

The app signs; the website stores the message and the worker publishes it unchanged.

```json
{"kid":"<signer kid>","p":"<base64url payload>","s":"<base64url signature>"}
```

Payload:

```json
{"v":1,"id":"<unique command id>","dev":"<device id>","iat":<now>,"exp":<now + 30>,
 "cmd":{"cmd":"start_preheat","minutes":20}}
```

- Signed bytes: `HEARTH-CMD-1` ‖ `\n` ‖ payload bytes.
- `cmd` is exactly a `POST /api/cmd` body, and needs the same permission (§6).

The box, in this order, and never trusting the payload before step 2:

1. finds `kid` in its user table (else `unknown_signer`);
2. verifies the signature (`bad_signature`);
3. checks `dev` is itself (`wrong_device`);
4. refuses without SNTP time (`no_clock`): a box that cannot tell the time cannot tell a stale command. A clock set only by a phone (`set_time`) does **not** count here, because a phone can be wrong; the box needs network time from this boot;
5. checks `iat − 60 ≤ now ≤ exp` and `exp − iat ≤ 300` (`expired`);
6. checks `id` is not among the last 64 command ids it has seen (`replay`). The ids are kept in RAM, so a restart forgets them; the 300 s lifetime and the clock rule of step 4 are what then limit a repeat;
7. checks the permission (`forbidden`);
8. hands it to the safety layer, which can still refuse (`refused`, with its reason).

### 9.2 Acknowledgements (`ack`, box to cloud)

`{"id":"<command id>","result":"accepted"|"done"|"rejected","reason":"<code above, or the safety layer's>"}`

The box answers every command it can name with exactly one ack: `done` once the safety layer has taken it, or `rejected` with one of the reason codes `malformed`, `unknown_signer`, `bad_signature`, `wrong_device`, `no_clock`, `expired`, `replay`, `forbidden` (steps 1 to 7 above) or `refused` (step 8: the safety layer said no, or the command name or value is not valid; the box also publishes an `event` `{"kind":"refused","id":…}`). The command is carried out inside the box before the ack, so `accepted` is not used. For a message whose signature did not check out the `id` is read from the payload **without trust**, only so the refusal can name it: nothing else in such a payload is used, and a message with no readable `id` gets no ack.

### 9.3 Status, state and events

- `status`: the plain text `online`, retained, QoS 1; the broker publishes `offline` as the box's Last Will. A deliberate stop (the link removed with `DELETE /api/cloud`) publishes `offline` itself first, since a clean disconnect does not fire the will.
- `state`: the same JSON as `GET /api/status`, plus `"firmware":"<version>"` as its first field (and `"ota_error":"<code>"` after it when the last remote update failed, §9.7), retained, QoS 1. Sent on connect, whenever the run state, fault code, run mode, burner or Auto changes, and at least every 15 s.
- `backup`: the `GET /api/backup` document (§7.5), retained, QoS 1. Sent on connect, within a minute of any setting changing (firmware 1.41), and within a second of the timers changing by any route (firmware 1.44). Its `timers` array is the copy the website gives apps that are away from the box (§9.9).
- `event`: `{"kind":"fault"|"refused"|"local_start"|"local_stop"|…, …}`. Firmware 1.0030 sends `fault` (`code`, `name`, once when a fault appears) and `refused` (`id`). `local_start` and `local_stop` are reserved and not sent yet.

### 9.4 Keys the cloud knows

The website keeps each enrolled box's admin public keys and the invited keys the owner has registered (`hth_grants`), so it can drop commands from unknown signers before they reach the broker. It is a filter only; the box decides.

### 9.5 The cloud client: connecting, retrying, endpoints

- **Opt-in.** Nothing runs until a certificate is installed (§8). `DELETE /api/cloud`, or installing a new certificate, takes the connection down at once (saying `offline` first) and a new one is made if a certificate is still installed.
- **Dials only with a certificate, a network and network time.** TLS dates cannot be checked without the clock, so the client waits for SNTP (not a phone-set clock) before the first attempt.
- **And only with memory for TLS** (1.0045): at least 30000 bytes of free heap and a 12288-byte largest block (`AT_CLOUDLINK_DIAL_*`). Short of that the state is `low_memory` and the client looks again each second; nothing was tried, so it is not a failure and the backoff and endpoint stay as they were. A handshake started without the memory fails part-way and leaves the heap more broken up, which on 1.0044 ran to eleven failures in a row and `/api/status` answering 500.
- **TLS.** Mutual: the box presents its enrolled certificate chain and checks the broker against the pinned Tillerwell Hearth Root CA, with the host name check on. The name checked is the host dialled, so the broker certificate must carry every entry of the list: the two domains today, and the reserved IP as an IP subject alternative name before that entry is filled in. MQTT 3.1.1, clean session, keep-alive 60 s, QoS 1.
- **Endpoint list** (cloud plan §9), tried in this order, port 8883: `mqtt.tillerwell.com`, `mqtt.tillerwell.co.uk`, then the standby's reserved IP. The third entry is empty (the address does not exist yet) and is skipped; the list is `AT_CLOUDLINK_HOST_*` in `components/at_cloudlink/include/at_cloudlink.h`.
- **N = 3.** After 3 failed attempts in a row on one endpoint the client moves to the next, and after the last it goes round again. An attempt fails if TLS or the MQTT connect is refused, or if no session is up 30 s after dialling.
- **Backoff.** After the f-th failure in a row the next attempt waits 5 s for the first six failures (the link is usually up within seconds of a restart), then 5 s × 2^(f−6), capped at 300 s, plus up to 25 % random spread. A connection that stayed up 60 s or more counts as a success, so the next drop starts again at 5 s on the endpoint then in use; a shorter one counts as one more failure.
- **Never in the way.** The client runs in its own tasks, below the thermostat and the heater bridge. Commands are verified and carried out one at a time in the client's supervisor task, not in the MQTT task; at most 4 wait, and a fifth is dropped. A heater with no account, no internet or a dead broker behaves exactly as before.
- **Diagnostics.** `GET /api/info` carries a `cloud` object: `state` (`off`, `waiting`, `low_memory`, `connecting`, `online`, `backoff`, and `paused` or `updating` while an update check or download has the memory), `endpoint`, `failures`, `connects`, `commands_ok`, `commands_refused`, `heap_at_connect` and `heap_min` (lowest free heap since boot, sampled while the client runs).

### 9.6 Signed requests to the website (apps away from home)

The apps hold no website login. When a phone cannot reach its box on the local network it reads the box's last state and manages cloud grants with requests signed by the **same user key** the box knows (an admin key from enrolment, §8, or an invited key the website has registered, §9.4). There is no nonce round trip; a timestamp limits replay instead.

String to sign, five lines joined with `\n`, no trailing newline:

```
HEARTH-READ-1
<device id>
<method, upper case>
<path: /api/m/hearth/public/devices/<device id>/…, no query string>
<ts: Unix seconds, the phone's clock>
```

Header: `Hearth-Auth: v1 kid=<kid> ts=<ts> sig=<base64url signature>`. The website accepts it when `|now − ts| ≤ 120` and the signature verifies with a key it knows for that device (refusals: `auth_required`, `expired`, `unknown_signer`, `bad_signature`, all 401).

| Route | Needs | Answer |
|---|---|---|
| `GET …/devices/<id>/state` | any known key | `{"online","last_seen_at","state","state_at","firmware_version","perm"}`; `state` is the box's last `state` message (§9.3) |
| `DELETE …/devices/<id>/grants/<kid>` | an admin key | `{"revoked":"<kid>"}`; the key's cloud commands are refused from then on |

Commands still go as §9.1 messages to `POST …/devices/<id>/commands`; `GET …/commands/<command id>` gives the outcome. The website decides nothing about the heater: a refused read only stops the phone seeing state through the cloud.

### 9.7 Reboot and firmware update from away (firmware 1.0032)

Both are admin commands in the §9.1 envelope. A phone away from the box's network can read the running version (the `firmware` field of `state`, also `firmware_version` in the website's state reply), restart the box, and install a release.

- `{"cmd":"reboot"}` (also accepted by `POST /api/cmd`): the box acks `done` and restarts 1.5 s later. It is back on the cloud once Wi-Fi and the broker reconnect (about a minute).
- `{"cmd":"auto_update","enabled":true|false}` (admin; also accepted by `POST /api/cmd`): turns automatic firmware updates on or off. `auto_update` in the state reports it. With it on, the box asks GitHub for the latest release every six hours and installs a newer image through the same checks as `ota` (hash and size from the release, release URL only), but only when the clock is set, the heater is idle, no timer is running and the next timer start is more than 30 minutes away. It tries again every minute until those hold. Off by default. The check reads the release JSON only up to the end of `assets` (8 KB at most) and runs when there are 48 KB free and an 18 KB largest block with the cloud link paused (1.0045; before that it wanted one 65 KB block, which a box never had, so it never got as far as asking).
- `{"cmd":"mqtt","enabled":true|false}` (admin; also accepted by `POST /api/cmd`, from 1.0046): turns the built-in MQTT broker and the box's MQTT client on or off at once, and keeps it across restarts. `mqtt` in the state reports it. Off by default: Home Assistant's Tillerwell integration uses HTTP, and the broker is an unsigned way in for anyone on the network.
- `{"cmd":"ota","version":"1.0032","url":"https://github.com/Tillerwell/tillerwell-hearth-at5/releases/download/<tag>/hearth-1.0032.bin","sha256":"<64 lower-case hex>","size":2400000}`. The box fetches the image itself over HTTPS (certificates checked against the standard root bundle). **The admin's signature on the command is what authorises the image, not the URL**: the box installs the image only if its SHA-256 and size equal the signed values, and the app takes both from the release asset's `digest` and `size` in GitHub's API.
  - Checked before acking, in this order, with `rejected` and the reason: `malformed` (fields missing or the wrong shape), `bad_url` (anything but `https://github.com/Tillerwell/tillerwell-hearth-at5/releases/download/<tag>/hearth-<version>.bin`, with the same version), `older` (no remote rollback; a re-install of the same version is allowed; use `POST /api/ota?allow_older=1` on the local network to roll back), `too_big` (larger than the update partition), `busy` (one update at a time).
  - Otherwise the box acks `done` (meaning accepted), says `offline` and drops the cloud link to free the memory for the download's own TLS session, downloads, checks the image's version string against the command, its size and its SHA-256 (read back from flash before the image can be chosen to boot), then boots it. There is no ack at the end: the app sees `state.firmware` change when the box reconnects.
  - If the update fails the box stays on the old image, reconnects, includes `"ota_error":"<code>"` in `state` until the next restart and sends the `event` `{"kind":"ota_failed","name":"<code>"}`. Codes: `download_failed`, `wrong_version`, `size_mismatch`, `bad_hash`, `bad_image`, `no_memory`.
  - The local route `POST /api/ota` (§5.2) is unchanged and is still how a phone on the same network sends an image.

### 9.8 Heater information and unlock (firmware 1.42)

Requests the vendor's test tool makes (docs/PROTOCOL.md, "Vendor test utility decoded"). Plain `POST /api/cmd` bodies, so they also go through the cloud as §9.1 commands.

- `{"cmd":"heater_read"}` (control): asks the heater for its version (`type 06`), its settings block (`type 20`) and its operation report (`type 0b`, four 64-byte pages). Six short diverts, about 3 s in all. The box acks at once; the answers show up in `heater_info` as they arrive, with `reading` true until the last one is in. Refused (`refused`) with Bypass on or the command queue too full for all six.
- `{"cmd":"heater_unlock"}` (manager): `type 0d`, which clears the heater's lockout (state `1.1`, "locked", after repeated failed starts or overheats). Only while the heater is idle or locked, its status is fresh and no Hearth run is going; otherwise `refused`. It does not clear a fault code, which stays until the next good ignition. There is no command that restarts the heater.
- `{"cmd":"diag_stream","enabled":true|false}` (admin): the vendor tool's switch for the extended frame. Experimental.

Firmware 1.43 adds the heater's own service functions, again exactly as the vendor tool sends them:

- `{"cmd":"fuel_prime"}` (control): `type 12`, the tool's FuelPump button. Runs the fuel pump without lighting, to push diesel through and clear the air after the tank ran dry. `{"cmd":"stop"}` ends it.
- `{"cmd":"fan_test"}` (manager): `type 08 60`, spins the blower without lighting. `stop` ends it.
  Both only while the heater is idle (state 0), its status fresh, the box's thermostat (Auto) off and no Hearth run going; otherwise `refused`.
- `{"cmd":"start_pump","minutes":n}` (control, was admin): the water (circulation) pump alone, `type 21` with the run time as a big-endian u16, 1-1440. Without `minutes` it uses the saved `pump_minutes` (default 40, the tool's own). This is **not** the fuel pump: earlier notes and the box's own page called it "pump only" and "ventilation", wrongly.
- `{"cmd":"pump_minutes","value":n}` (manager): the saved water pump run time, 1-1440. In the status and in the backup (§7.5) as `pump_minutes`.
- `{"cmd":"heater_settings","wait_temp":20-95,"pump_standby":true|false}` (manager): changes the heater's own coolant target and water pump in standby; either field may be left out. Writes `type 20` with the block exactly as the heater last gave it except those two bytes, then reads it back, so `heater_info.settings` shows what the heater now holds. Needs a settings block first (`heater_read`, or the controller's own read at power-up); `refused` without one or with a value out of range. **This writes the heater's own memory.**

`heater_info.report.faults` (1.43): the heater's fault history, newest first, empty entries left out:

```json
"faults":[{"code":13,"name":"No ignition","repeats":3,"mode":"Ignition 1","run_s":256,"mode_s":12,"voltage":12.5,
  "flame_temp":40,"liquid_temp":40,"overheat_temp":41,"board_temp":22,"second_attempts":1,"flame_breaks":0}]
```

Ten entries of 17 bytes from report byte 55; the report's last fault column says which is newest. The layout is the vendor profile's fault table and, like the rest of the report, not yet checked on a real heater.

`heater_info` in `GET /api/status` (and so in the cloud `state`) appears once anything has been read or overheard (the controller reads the settings block itself at power-up):

```json
"heater_info":{"reading":false,"read_age_s":12,"version":"10.1.3.7","device_type":10,
 "settings":{"wait_temp":88,"warmup":"off","warmup_wait_temp":85,"relay":"off","relay_on_temp":40,
   "pump_signal":"constant","pump_with_engine":false,"pump_standby":true,"signal_channel":true,"age_s":12},
 "report":{"product_code":0,"serial":0,"starts":0,"starts_ok":0,"starts_fault":0,"first_attempt":0,
   "second_attempt":0,"no_ignition":0,"power_cuts":0,"water_pump_runs":0,"warmup_runs":0,"total_s":0,
   "high_s":0,"middle_s":0,"low_s":0,"wait_s":0,"pump_only_s":0,"last_fault_column":0,"age_s":12},
 "unlock_age_s":40,"unlock_replied":true}
```

Every member is optional: `version` only once the heater has answered, `settings` once a settings block is known, `report` once all four pages are in, `unlock_*` after an unlock. `settings.wait_temp` is the heater's own coolant target (in preheat it stops burning there and waits). The report's field offsets come from the tool's profile and are **not yet checked against a real heater**; `GET /api/info` carries `heater_raw` (`settings` hex, `page_len`, the 256 `eeprom` bytes as hex) for that check. Read only: nothing here writes to the heater.

### 9.9 Timers from away (firmware 1.44)

Only for a box linked to the cloud (§8): nothing here exists without enrolment. A whole timer list (up to 32, about 5 KB) does not fit in a §9.1 command, so the cloud changes one timer at a time. Both are plain `POST /api/cmd` bodies too, and need **control** (§6):

- `{"cmd":"timer_set","timer":{…}}`: `timer` is one object in the `GET /api/timers` format (docs/TIMERS.md). It replaces the timer with the same `id`, or is added at the end. The whole resulting list goes through the same rules as `PUT /api/timers`; if it breaks one, nothing changes and the command is `refused`.
- `{"cmd":"timer_delete","id":"…"}`: removes that timer; `refused` if there is none.

The box sends a fresh `backup` (§9.3) as soon as its list changes, so the website's copy follows changes made at home, from away and by the box itself (a finished one-off dropping off). The website returns that copy as `timers` and `timers_at` in `GET …/devices/<id>/state` (§9.6).

### 9.10 Heater Care: the working range, run locks and learning (firmware 1.47)

Plan and reasons: `docs/ADAPTIVE_THERMOSTAT.md`. The user's range (`auto_low`..`auto_high`, at least 1 C) is what the
apps show as the setting. The thermostat runs on a working range: the same top, with the bottom lowered by what
Heater Care learned (on the controller's whole-degree sensor the learned width rounded up to a whole degree plus 1 C,
and at least 2 C below the top). Hearth's own stops
wait until a run has burned 30 minutes from flame established (20 for frost protection); a thermostat start waits
until the heater has been idle 20 minutes. A fault, the ceiling (top + 5 C) and a stop someone asks for always go at
once.

`GET /api/status` (and the MQTT and cloud `state`) gains a `thermo` object:

```json
"thermo": {
  "work_low": 19.5, "work_high": 21.0, "learned_width": 1.5, "min_width": 1.0, "sensor": "external",
  "ceiling": 26.0, "at_limit": null, "reason": "widened",
  "min_run_left_s": 480, "min_off_left_s": 0, "holding": null,
  "run_on_at": 1762871100, "flame_on_at": 1762871400,
  "sensor_stuck": false, "no_reading": false,
  "rise_rate": 0.05, "warmup_typical_s": 360, "cool_typical_s": 2700, "dip_below": 0.8,
  "early_stop_unseen": 1, "last_run_end": "top",
  "last_early_stop": {"at": 1762871940, "after_s": 540, "source": "matter"}
}
```

- `sensor`: `"external"` or `"controller"`, the reading in use. `min_width` is the narrowest working range on it (1 or 2).
- `at_limit`: `"max"` (the space heats faster than a 4 C range allows), `"min"` (at the narrowest; normal, say nothing) or null.
- `learned_width`: the real swing the space needs, the same number on both sensors. On the controller's sensor the working range is it rounded up to a whole degree plus 1 C (the room there swings 1 C less than the range), at least 2 C.
- `reason`: `"user"` (working range = the user's), `"widened"`, `"narrowed"` (the learned width is wider than the user's range), `"controller_sensor"` (wider than the user's range only because of the controller's sensor: its 2 C floor, or its whole degrees plus 1 C).
- `min_run_left_s`: 0 when no run lock; the full 1800 (1200 for frost protection) all through light-up, then counting down from flame established. The apps' stop warning uses it.
- `min_off_left_s`: 0 when no off lock. The manual start warning uses it.
- `holding`: why a stop Hearth wants to send is waiting for the run lock: `"top"`, `"timer_ended"`, `"thermostat_off"`, `"no_reading"`, or null (`"top"` also while frost protection waits for its 20 minutes).
- `ceiling`: the stop point for the run going (frost protection's target + 1 + 5; while a timer's run finishes after the timer ended, the timer's top + 5). With no run going: the ceiling the last run stopped at when `last_run_end` is `"ceiling"`, otherwise the range's top + 5. It is always 5 C above where Hearth would have stopped that run.
- A stop Hearth sends is sent again every 90 s while the heater still reports starting or burning, until it purges or goes idle.
- `run_on_at`, `flame_on_at`: epochs of the current run's start and flame, null when not running, lighting up, or the clock is not set.
- `run_adopted` (1.51): true while the run was already going when the box restarted and its true start is not known, so the run lock counts from boot. Absent before 1.51 (treat as false).
- `sensor_stuck`: the external sensor has not changed for 15 minutes while heating, while the controller's reading rose 2°C, and is set aside until its next new value (1.50+; before that, 15 flat minutes alone). `no_reading`: no usable temperature at all.
- `rise_rate`, `warmup_typical_s`, `cool_typical_s`: for the sensor in use, null until known. `dip_below`: how far below the bottom the room fell during the current or last off lock.
- `early_stop_unseen`: stops someone asked for inside the minimum run that no app has explained yet; `last_early_stop` is the latest one (`source`: `app`, `web`, `matter`, `mqtt`, `home_assistant`, `controller`, `console`, `timer`, `unknown`). Ceiling stops and faults never count.
- `last_run_end`: `"top"`, `"user"`, `"external"`, `"ceiling"`, `"fault"`, `"no_reading"`, `"heater"`, `"timer_ended"`, `"thermostat_off"`, or null.

`hyst_min` and `hyst_max` stay in the status for older apps, always 0.5 + 0.5, so an app that adds them up gets a 1 C minimum.

`GET /api/thermo/history` (view):

```json
{"learned_width":1.5,"sensor":"external","limit_ext":null,"limit_ctl":null,
 "history":[{"at":1762871100,"kind":"widened","sensor":"external","limit":null,"old":1,"new":1.5,
             "worked_out":1.21,"working":1.5,"rate1":0.05,"rate2":0.048}],
 "runs":[{"at":1762871100,"sensor":"external","end":"top","counted":true,"why":"counted","vote":"fast",
          "light_s":300,"burn_s":1900,"warmup_s":360,"rate":0.05,"need":1.7,"climb_min":26,
          "start_temp":19.9,"peak_temp":21.6,"cooling_s":2700,"dip":0}],
 "early_stops":[{"at":1762871940,"after_s":540,"source":"matter"}]}
```

Newest first. In a `history` entry `old` and `new` are the learned width (the real swing) and `working` is the
working width on that entry's sensor (on the controller's, the learned width in whole degrees plus 1 C). `history`
(kinds `widened`, `narrowed`, `limit_reached`, `limit_cleared`, `reset`) is kept across a restart; `runs` (the last 20) and `early_stops` (the last 8) are not. `why` is `counted`, `not_thermostat`,
`long_off`, `not_at_top`, `sensor_changed`, `lost_heat`, `not_enough_data` or `cold_spell`.

Commands (`POST /api/cmd`, and the cloud `cmd`):

- `{"cmd":"thermo_learn_reset"}` (manager): forget what was learned, with a history entry.
- `{"cmd":"early_stop_seen"}` (control): an app showed the short-cycle explainer.
- **One set temperature (1.49).** The user sets one temperature, not a range. It is the top of the range (`auto_high`, also reported as `auto_target`), and `auto_low` is always 1 C below it. `{"cmd":"auto_range","low":…,"high":…}` uses only `high` and ignores `low`; `{"cmd":"auto_target","value":…}` sets the same thing. A range saved by older firmware loads as its top. Timers do the same with `high_c`. Apps send `low = high - 1`, which also works on 1.47 and 1.48.
- `{"cmd":"auto","enabled":false,"finish_run":true|false}` and `{"cmd":"auto_range","low":…,"high":…,"finish_run":…}`: inside a run's minimum run, `true` applies the change and lets the run reach it; `false` stops now and is logged as an early stop. Older apps send neither, which is treated as `true`.
- `stop`, `auto` and `auto_range` take an optional `"source"` for the early-stop log (`"app"`, `"home_assistant"`, …). Without it, `POST /api/cmd` counts as `web` and a cloud command as `app`.
- `{"cmd":"hysteresis",…}` is deprecated: accepted and ignored, answering `{"ok":true,"skipped":true}`.

### 9.11 The heater's cooldown (firmware 1.52)

`GET /api/status` (and the MQTT and cloud `state`) reports the heater's own cooldown (status 0x04 and 0x05 before idle):

- `cooling`: the heater is cooling down now.
- `cooldown_at`: wall clock (epoch seconds) when the box first saw this cooldown; null when not cooling, when it was already cooling at boot, or before the clock is set. Apps count down from here, so the timer starts when the heater stopped, not when an app looked.
- `cooldown_elapsed_s`: the same as seconds since it began (null as above); for a client without a trustworthy clock.
- `cooldown_typical_s`: how long a cooldown usually takes on this heater: the median of the last 10 seen whole (30 s to 30 min, from 0x04/0x05 to idle; a relight cuts one short and it is not kept), or 300 until 2 are known.
- `cooldown_learned`, `cooldown_samples`: whether the typical is learned, and from how many. Kept across a restart (NVS `ctl_cool`); not part of Reset learning, since it belongs to the heater, not the space.

### 9.12 Stopping early by the rise after a stop (firmware 1.53)

After Hearth's thermostat stops the heater at the top, the room usually keeps rising for a while (heat in the heater, its cooldown fan, the room itself). Each stop at the top on the external room sensor is followed: the highest reading after it, less the reading at the stop, is kept once the room has fallen 0.3 C below that peak (not kept if the controller's sensor takes over, a new run starts first, there is no fall within 3 hours, or the rise is over 3 C). From 3 of them, the thermostat stops early by their median (of the last 7), capped at 1 C and at half the working range, in 0.1 C steps rounded down, so the peak lands on the set temperature. The 30-minute minimum run still applies. Never on the Comfort Controller's whole-degree sensor. Added to `thermo`:

- `stop_at`: the reading the thermostat stops at (`work_high` less `stop_early`).
- `stop_early`: how far below the top it stops, 0 until learned or on the controller's sensor.
- `coast_typical`: the typical rise after a stop at the top, C; null until 3 are known.
- `coast_samples`: how many are kept. Kept across a restart (NVS `ctl_coast`); cleared by Reset learning, since it belongs to the space.

## 10. Test vectors

`docs/auth-vectors.json` (generated by `host_tests/gen_auth_vectors.py`) holds:

- three test keys (`admin`, `guest`, `outsider`) derived from **published** seeds: never use them for anything real;
- signed requests, valid ones and ones tampered after signing (path, body, signed for another box);
- grants: valid, permission raised after signing, signed by a non-admin; each with its QR text;
- cloud commands: valid, changed after signing, expired, for another box.

Every implementation should reproduce the valid signatures byte for byte (deterministic signing) and give the stated result for each case. The firmware does so in `host_tests/test_auth.c` (`make -C host_tests auth`, 100 checks, using ESP-IDF's own mbedTLS 3.6.6 and cJSON). `host_tests/test_access.c` (`make -C host_tests access`) runs the whole §9.1 check order on the cloud-command vectors, plus the lifetime edges, replay and permissions; `make -C host_tests cloudlink` checks the §9.5 backoff and endpoint walk.

## 11. Decisions made with this spec

Not settled in the planning docs before; all agreed by Chris on 2026-10-28, including the one-step invite (3):

1. **Device id** becomes the full MAC (`hearth-a0b1c2d3e4f5`). The current 3-byte name is not unique across a fleet.
2. **Raw `r ‖ s` signatures**, not DER.
3. **The invite QR carries the guest's private key** (the admin's app makes the key pair, as the cloud plan describes). It is shown in person and is one-time, but anyone who photographs it before it is redeemed could redeem it first. The alternative is a two-step invite (the guest's app makes its own key and shows a QR back to the admin), which is safer but needs both phones to scan each other.
4. **16 users**, **16 outstanding nonces**, **32 remembered invites**, **64 remembered command ids**.
5. **Who may do what:** filling the tank and setting the tank level are *control*; changing the tank size, fuel prices and rates, time zone and the box's name are *manager*; firmware updates are *admin*.
6. **Cloud commands live at most 300 seconds** (normally 30, cloud plan Q4), with 60 seconds of clock slack.
7. **L2 serves the signed API on port 80** (§3) and adds `POST /api/auth/legacy` (§3); the claim button is a short press of the existing button (§7.1); an unsigned request to any `/api/auth/` route that needs a user is `401 auth_required` even while the old API is on.
8. **Cloud client (C4, firmware 1.0030; chosen while building it, not yet reviewed by Chris):** N = 3 attempts per endpoint; backoff 5 s doubling to 300 s; a phone-set clock does not satisfy §9.1 step 4; `done` or `rejected` acks only (no `accepted`); the replay ids are RAM only (§9.1 step 6); the reserved-IP entry stays empty until that address exists (§9.5).
9. **Memory (1.0031):** the unified image had 12 to 17 KB of free heap and a 3 KB largest block with the cloud link idle, so a TLS session (about 20 KB of buffers) could not start. `sdkconfig.defaults.all` now keeps Wi-Fi and system code out of fast RAM, allocates TLS buffers only while a session needs them and halves the Bluetooth pools: 56 to 110 KB free, 44 KB largest block, 22 KB lowest, 52 KB free with a session up (one bench heater, 2026-10-28). The broker's own host name joined the endpoint list (§9.5).
10. **Memory (1.0045):** a 1.0044 box with the cloud link retrying had 27 to 38 KB free but a 2.5 to 8 KB largest block, and 12 bytes at its lowest. `/api/status` then answered 500, because printing it needed a 3 KB block and a bigger one alongside it once `heater_info` passed 3 KB. The status document is now printed into one 6 KB buffer reserved at boot and served from it without a copy; the cloud link does not dial short of memory (§9.5); `GET /api/info` gains `reset_reason` (`power_on`, `software`, `panic`, `task_watchdog`, `brownout` and so on) and `uptime_s`. The limits are chosen from that one box's numbers, not yet measured across a handshake on the bench.
11. **Memory (1.0046):** right after the 1.0045 update the box had 11 to 43 KB free, a 3 KB largest block and 12 bytes at its lowest, and answered few requests. Measured causes, largest first: Bluetooth setup held about 30 KB for the ten minutes after every start, updates included (free heap rose by 32 KB the moment it closed); the built-in MQTT broker and client were always on and republished the state document every 2 s, copying it three or four times each go, though Home Assistant uses HTTP; and the Wi-Fi driver could hold 32 receive and 32 transmit buffers of 1.6 KB in a burst. Now Bluetooth opens its window only after a real power-up or with no Wi-Fi saved (still whenever Wi-Fi has been gone 30 s, see BLE_SETUP.md); MQTT is off unless an admin sends `{"cmd":"mqtt","enabled":true}` (`mqtt` in the state); the Wi-Fi driver keeps 8 static and at most 16 dynamic receive buffers and 16 transmit buffers; the status document is rebuilt at most once a second. `GET /api/info` gains `boot_heap` (free and largest block after each start-up step: `start`, `heater`, `network`, `web`, `bluetooth` or `matter`, `cloud`, `mqtt`), `bluetooth_on` and `mqtt_on`, and `last_crash` after a crash: `why` (the abort or assert message, else the exception), `task`, `pc`, `ra`, `sp`, `mcause`, `mtval`, `stack` (code addresses found on the crashed task's stack), `uptime_s`, `crashes` since power was connected and `boots_since` (0: the last restart was this crash). The record lives in RTC memory, written by a wrapper round ESP-IDF's panic handler (there is no core-dump partition, and an update cannot add one), and goes when the power does. Each release carries `hearth-<ver>.elf.gz` to decode the addresses: `riscv32-esp-elf-addr2line -pfiaC -e hearth-<ver>.elf <pc> <ra> <stack...>`. Not changed: about 57 KB of static RAM belongs to Matter and Thread and is held in Wi-Fi mode too, because one image carries all three modes.
