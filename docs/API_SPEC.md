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
| `GET /api/status`, `/api/info`, `/api/timers`, `/api/fuel`, `/api/auth/me` | view |
| `PUT /api/timers` | control |
| `POST /api/cmd` | depends on `cmd`, below (at least control) |
| `GET /api/wifi/scan`, `POST /api/wifi`, `POST /api/mode` | manager |
| `/api/auth/users…`, `/api/cloud…`, `POST /api/ota`, `POST /api/auth/grant-revoke` | admin |
| anything else | admin (fail closed) |

`POST /api/cmd`:

| Needs | Commands |
|---|---|
| control | `start_preheat`, `stop`, `auto`, `auto_target`, `set_time`, `tank_level`, `tank_filled` |
| manager | `auto_range`, `hysteresis`, `preheat_temp`, `preheat_minutes`, `timezone`, `display_name`, `fuel_price`, `fuel_factor`, `fuel_rates`, `tank_capacity` |
| admin | `factory_reset`, and **any command not listed** (fail closed) |

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
- `state`: the same JSON as `GET /api/status`, plus `"firmware":"<version>"` as its first field, retained, QoS 1. Sent on connect, whenever the run state, fault code, run mode, burner or Auto changes, and at least every 15 s.
- `event`: `{"kind":"fault"|"refused"|"local_start"|"local_stop"|…, …}`. Firmware 1.0030 sends `fault` (`code`, `name`, once when a fault appears) and `refused` (`id`). `local_start` and `local_stop` are reserved and not sent yet.

### 9.4 Keys the cloud knows

The website keeps each enrolled box's admin public keys and the invited keys the owner has registered (`hth_grants`), so it can drop commands from unknown signers before they reach the broker. It is a filter only; the box decides.

### 9.5 The cloud client: connecting, retrying, endpoints

- **Opt-in.** Nothing runs until a certificate is installed (§8). `DELETE /api/cloud`, or installing a new certificate, takes the connection down at once (saying `offline` first) and a new one is made if a certificate is still installed.
- **Dials only with a certificate, a network and network time.** TLS dates cannot be checked without the clock, so the client waits for SNTP (not a phone-set clock) before the first attempt.
- **TLS.** Mutual: the box presents its enrolled certificate chain and checks the broker against the pinned Tillerwell Hearth Root CA, with the host name check on. The name checked is the host dialled, so the broker certificate must carry every entry of the list: the two domains and the VPS host name today, and the reserved IP as an IP subject alternative name before that entry is filled in. MQTT 3.1.1, clean session, keep-alive 60 s, QoS 1.
- **Endpoint list** (cloud plan §9), tried in this order, port 8883: `mqtt.tillerwell.com`, `mqtt.tillerwell.co.uk`, the broker's own host name `vps-de870a75.vps.ovh.ca` (added in 1.0031: it resolves without any DNS record of ours, so a box can still connect while the two domains are not set up), then the standby's reserved IP. The fourth entry is empty (the address does not exist yet) and is skipped; the list is `AT_CLOUDLINK_HOST_*` in `components/at_cloudlink/include/at_cloudlink.h`.
- **N = 3.** After 3 failed attempts in a row on one endpoint the client moves to the next, and after the last it goes round again. An attempt fails if TLS or the MQTT connect is refused, or if no session is up 30 s after dialling.
- **Backoff.** After the f-th failure in a row the next attempt waits 5 s × 2^(f−1), capped at 300 s, plus up to 25 % random spread. A connection that stayed up 60 s or more counts as a success, so the next drop starts again at 5 s on the endpoint then in use; a shorter one counts as one more failure.
- **Never in the way.** The client runs in its own tasks, below the thermostat and the heater bridge. Commands are verified and carried out one at a time in the client's supervisor task, not in the MQTT task; at most 4 wait, and a fifth is dropped. A heater with no account, no internet or a dead broker behaves exactly as before.
- **Diagnostics.** `GET /api/info` carries a `cloud` object: `state` (`off`, `waiting`, `connecting`, `online`, `backoff`), `endpoint`, `failures`, `connects`, `commands_ok`, `commands_refused`, `heap_at_connect` and `heap_min` (lowest free heap since boot, sampled while the client runs).

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
