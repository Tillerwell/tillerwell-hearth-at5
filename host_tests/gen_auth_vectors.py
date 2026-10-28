#!/usr/bin/env python3
"""Generate the signing test vectors for docs/API_SPEC.md.

Writes docs/auth-vectors.json (for the iOS and Android apps and the website) and
host_tests/auth_vectors.h (for the firmware host test). Every key is derived from
a fixed, published test seed and every signature is deterministic (RFC 6979), so
any implementation can reproduce these bytes exactly. NEVER use these keys for
anything real.

usage: python3 host_tests/gen_auth_vectors.py   (from the repo root)
"""
import base64, hashlib, json, os, sys

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
N = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551  # P-256 order

DEVICE_ID = "hearth-a0b1c2d3e4f5"
OTHER_DEVICE_ID = "hearth-0f0e0d0c0b0a"
NOW = 1793203200  # 2026-10-28T12:00:00Z


def b64u(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def test_key(label: str):
    scalar = int.from_bytes(hashlib.sha256(f"tillerwell-hearth-test-key:{label}".encode()).digest(), "big") % N
    return ec.derive_private_key(scalar, ec.SECP256R1())


def pub_bytes(key) -> bytes:
    nums = key.public_key().public_numbers()
    return b"\x04" + nums.x.to_bytes(32, "big") + nums.y.to_bytes(32, "big")


def priv_bytes(key) -> bytes:
    return key.private_numbers().private_value.to_bytes(32, "big")


def key_id(pub: bytes) -> str:
    return b64u(hashlib.sha256(pub).digest()[:8])


def sign(key, msg: bytes) -> bytes:
    der = key.sign(msg, ec.ECDSA(hashes.SHA256(), deterministic_signing=True))
    r, s = decode_dss_signature(der)
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


def string_to_sign(device_id, method, path, nonce, body: bytes) -> bytes:
    return "\n".join(["HEARTH-REQ-1", device_id, method, path, nonce, hashlib.sha256(body).hexdigest()]).encode()


def envelope(prefix: str, key, payload_obj) -> dict:
    payload = json.dumps(payload_obj, separators=(",", ":")).encode()
    sig = sign(key, prefix.encode() + b"\n" + payload)
    return {"payload_bytes": payload, "p": b64u(payload), "s": b64u(sig)}


keys = {name: test_key(name) for name in ("admin", "guest", "outsider")}
pub = {n: pub_bytes(k) for n, k in keys.items()}
kid = {n: key_id(p) for n, p in pub.items()}

vectors = {
    "_comment": "Test vectors for docs/API_SPEC.md. Keys come from published seeds: never use them for anything real.",
    "device_id": DEVICE_ID,
    "now": NOW,
    "keys": {
        n: {"private_hex": priv_bytes(k).hex(), "public_b64url": b64u(pub[n]), "key_id": kid[n]}
        for n, k in keys.items()
    },
    "requests": [],
    "grants": [],
    "commands": [],
}

# --- Signed local HTTP requests (spec §5) -----------------------------------
def add_request(name, signer, method, path, body: bytes, nonce, expect, device_id=DEVICE_ID, tamper=None):
    sts = string_to_sign(device_id, method, path, nonce, body)
    sig = sign(keys[signer], sts)
    header = f"v1 kid={kid[signer]} nonce={nonce} sig={b64u(sig)}"
    case = {
        "name": name, "signer": signer, "method": method, "path": path, "body": body.decode(),
        "nonce": nonce, "string_to_sign": sts.decode(), "signature_b64url": b64u(sig),
        "header": header, "expect": expect,
    }
    if tamper:  # the box sees a different request from the one that was signed
        case.update(tamper)
    vectors["requests"].append(case)


nonce1 = b64u(hashlib.sha256(b"nonce-1").digest()[:16])
nonce2 = b64u(hashlib.sha256(b"nonce-2").digest()[:16])
add_request("guest reads status", "guest", "GET", "/api/status", b"", nonce1, "valid")
add_request("admin starts heater", "admin", "POST", "/api/cmd", b'{"cmd":"start_preheat","minutes":30}', nonce2, "valid")
add_request("path changed after signing", "admin", "POST", "/api/cmd", b'{"cmd":"stop"}', nonce2, "bad_signature",
            tamper={"path": "/api/timers"})
add_request("body changed after signing", "admin", "POST", "/api/cmd", b'{"cmd":"stop"}', nonce2, "bad_signature",
            tamper={"body": '{"cmd":"factory_reset"}'})
add_request("signed for another box", "admin", "GET", "/api/status", b"", nonce1, "bad_signature",
            device_id=OTHER_DEVICE_ID)

# --- Invitation grants (spec §7) --------------------------------------------
def add_grant(name, perm, expect, tamper_perm=None, signer="admin"):
    obj = {"v": 1, "typ": "grant", "gid": b64u(hashlib.sha256(name.encode()).digest()[:12]), "dev": DEVICE_ID,
           "key": b64u(pub["guest"]), "label": "Sam's phone", "perm": perm, "iat": NOW,
           "redeem_by": NOW + 900, "exp": 0, "by": kid[signer]}
    env = envelope("HEARTH-GRANT-1", keys[signer], obj)
    payload_b64 = env["p"]
    if tamper_perm:
        obj2 = dict(obj, perm=tamper_perm)
        payload_b64 = b64u(json.dumps(obj2, separators=(",", ":")).encode())
    token = f"{payload_b64}.{env['s']}"
    fp = b64u(hashlib.sha256(b"example box certificate SubjectPublicKeyInfo").digest())
    qr = f"hearth-invite:1:{DEVICE_ID}:{fp}:{token}:{b64u(priv_bytes(keys['guest']))}"
    vectors["grants"].append({
        "name": name, "signer": signer, "payload_json": base64.urlsafe_b64decode(payload_b64 + "=" * (-len(payload_b64) % 4)).decode(),
        "token": token, "qr": qr, "expect": expect,
    })


add_grant("admin invites guest with control", "control", "valid")
add_grant("perm raised after signing", "view", "bad_signature", tamper_perm="manager")
add_grant("signed by someone who is not an admin", "control", "unknown_signer", signer="outsider")

# --- Cloud commands over MQTT (spec §9) -------------------------------------
def add_command(name, signer, cmd, expect, iat=NOW, exp=NOW + 30, dev=DEVICE_ID, tamper_cmd=None):
    obj = {"v": 1, "id": f"cmd-{hashlib.sha256(name.encode()).hexdigest()[:12]}", "dev": dev, "iat": iat, "exp": exp, "cmd": cmd}
    env = envelope("HEARTH-CMD-1", keys[signer], obj)
    p = env["p"]
    if tamper_cmd:
        p = b64u(json.dumps(dict(obj, cmd=tamper_cmd), separators=(",", ":")).encode())
    message = json.dumps({"kid": kid[signer], "p": p, "s": env["s"]}, separators=(",", ":"))
    vectors["commands"].append({
        "name": name, "signer": signer, "payload_json": env["payload_bytes"].decode(), "message": message, "expect": expect,
    })


add_command("guest starts heater from the cloud", "guest", {"cmd": "start_preheat", "minutes": 20}, "valid")
add_command("command changed after signing", "guest", {"cmd": "stop"}, "bad_signature",
            tamper_cmd={"cmd": "factory_reset"})
add_command("expired command", "guest", {"cmd": "stop"}, "expired", iat=NOW - 120, exp=NOW - 90)
add_command("command for another box", "guest", {"cmd": "stop"}, "wrong_device", dev=OTHER_DEVICE_ID)

with open(os.path.join(ROOT, "docs", "auth-vectors.json"), "w") as f:
    json.dump(vectors, f, indent=2, default=lambda b: b.decode())
    f.write("\n")

# --- C header for host_tests/test_auth.c ------------------------------------
def c_str(s: str) -> str:
    return json.dumps(s)


out = ["/* Generated by gen_auth_vectors.py from the same data as docs/auth-vectors.json. Do not edit. */",
       "#pragma once", "#include <stdint.h>", "",
       f"#define AV_DEVICE_ID {c_str(DEVICE_ID)}", f"#define AV_NOW {NOW}LL", ""]
out.append("typedef struct { const char *name; const char *pub_b64url; const char *key_id; } av_key_t;")
out.append("static const av_key_t AV_KEYS[] = {")
for n in keys:
    out.append(f"  {{ {c_str(n)}, {c_str(b64u(pub[n]))}, {c_str(kid[n])} }},")
out.append("};\n")
out.append("typedef struct { const char *name; const char *signer; const char *method; const char *path; const char *body;"
           " const char *header; const char *string_to_sign; const char *expect; } av_request_t;")
out.append("static const av_request_t AV_REQUESTS[] = {")
for r in vectors["requests"]:
    out.append("  { " + ", ".join(c_str(r[k]) for k in ("name", "signer", "method", "path", "body", "header", "string_to_sign", "expect")) + " },")
out.append("};\n")
out.append("typedef struct { const char *name; const char *signer; const char *token; const char *qr; const char *expect; } av_grant_t;")
out.append("static const av_grant_t AV_GRANTS[] = {")
for g in vectors["grants"]:
    out.append("  { " + ", ".join(c_str(g[k]) for k in ("name", "signer", "token", "qr", "expect")) + " },")
out.append("};\n")
out.append("typedef struct { const char *name; const char *signer; const char *message; const char *expect; } av_command_t;")
out.append("static const av_command_t AV_COMMANDS[] = {")
for c in vectors["commands"]:
    out.append("  { " + ", ".join(c_str(c[k]) for k in ("name", "signer", "message", "expect")) + " },")
out.append("};")
with open(os.path.join(ROOT, "host_tests", "auth_vectors.h"), "w") as f:
    f.write("\n".join(out) + "\n")

print(f"{len(vectors['requests'])} requests, {len(vectors['grants'])} grants, {len(vectors['commands'])} commands", file=sys.stderr)
