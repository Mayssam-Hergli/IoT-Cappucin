"""EMBER attack test: what the jury will try, done by us first.
Run with the server up (board or fake_esp32 running is best):  python attack_test.py [server_url]
"""
import hashlib, hmac, json, random, sys, time

import requests, urllib3

from config import BASE_URL, EMBER_KEY, USERS

urllib3.disable_warnings()
BASE = (sys.argv[1] if len(sys.argv) > 1 else BASE_URL).rstrip("/")
GREEN, RED, BOLD, END = "\033[92m", "\033[91m", "\033[1m", "\033[0m"
results = []

def sign(p, key=EMBER_KEY):
    return hmac.new(key.encode(), p.encode(), hashlib.sha256).hexdigest()

def check(name, ok, detail):
    results.append(ok)
    tag = f"{GREEN}BLOCKED{END}" if ok else f"{RED}NOT BLOCKED{END}"
    print(f"  -> {tag}  {detail}\n")

def payload(boot, seq, state="NORMAL", level=0):
    return {"dev": "room1", "boot": boot, "seq": seq, "up_ms": 1000, "sim": True,
            "raw": {"gas": 1700, "temp": 24.5, "hum": 41.0, "ldr": 2100, "ir": 1},
            "feat": {"gas_delta": 0, "gas_slope": 0, "temp_slope": 0, "light_delta": 0, "flicker": 0},
            "cv": 0, "probs": {"normal": 0.98, "leak": 0.01, "fire": 0.01},
            "why": {"gas_delta": 0, "gas_slope": 0, "temp_slope": 0, "light_delta": 0, "flicker": 0},
            "level": level, "state": state, "cause": "none", "mode": "model", "latched": False,
            "silenced": False, "valve": "open", "faults": [], "tamper": False, "link_ok": True,
            "last_cmd": 0}

def dumps(o):
    return json.dumps(o, separators=(",", ":"))

print(f"{BOLD}EMBER attack test against {BASE}{END}\n")

# viewer session (used to learn the board's current boot, and for test 5)
viewer = requests.Session(); viewer.verify = False
viewer.post(BASE + "/login", data={"username": "viewer", "password": USERS["viewer"]["password"]})
st = viewer.get(BASE + "/api/state").json()
reading = st.get("reading") or {}

# 1) forged packet: attacker does not know the key
print(f"{BOLD}1. Forged reading (attacker signs with a wrong key){END}")
p = dumps(payload(12345, 999999, "NORMAL"))
r = requests.post(BASE + "/api/reading", json={"p": p, "sig": sign(p, "attacker-guess")}, verify=False)
check("forged", r.status_code == 401, f"HTTP {r.status_code} {r.text.strip()[:60]}")

# 2) tampered packet: valid signature, then attacker edits ALARM -> NORMAL
print(f"{BOLD}2. Tampered reading (real signature, payload changed ALARM -> NORMAL){END}")
p_real = dumps(payload(12345, 999999, "ALARM", 2))
sig_real = sign(p_real)
p_fake = p_real.replace('"ALARM"', '"NORMAL"').replace('"level":2', '"level":0')
r = requests.post(BASE + "/api/reading", json={"p": p_fake, "sig": sig_real}, verify=False)
check("tampered", r.status_code == 401, f"HTTP {r.status_code} {r.text.strip()[:60]}")

# 3) replay: re-send an old (validly signed) packet from the board's current session
print(f"{BOLD}3. Replay of a captured old packet{END}")
if reading.get("boot") is not None:
    old = payload(reading["boot"], 1)
    old["dev"] = reading.get("dev", "room1")
    p = dumps(old)
    r = requests.post(BASE + "/api/reading", json={"p": p, "sig": sign(p)}, verify=False)
    check("replay", r.status_code == 409,
          f"replayed seq=1 of live boot {reading['boot']} (board is at seq {reading.get('seq')}): HTTP {r.status_code}")
else:
    boot = random.randint(1, 2**31 - 1)
    p = dumps(payload(boot, 1))
    env = {"p": p, "sig": sign(p)}
    r1 = requests.post(BASE + "/api/reading", json=env, verify=False)
    r2 = requests.post(BASE + "/api/reading", json=env, verify=False)
    check("replay", r1.status_code == 200 and r2.status_code == 409,
          f"(no board online: test boot) first send HTTP {r1.status_code}, replay HTTP {r2.status_code}")

# 4) command without login
print(f"{BOLD}4. Fake command 'reset' without login{END}")
r = requests.post(BASE + "/api/command", json={"cmd": "reset"}, verify=False, allow_redirects=False)
check("nologin", r.status_code in (401, 302), f"HTTP {r.status_code} {r.text.strip()[:60]}")

# 5) command as viewer
print(f"{BOLD}5. 'reset' by a logged-in viewer (not allowed){END}")
r = viewer.post(BASE + "/api/command", json={"cmd": "reset"})
check("viewer", r.status_code == 403, f"HTTP {r.status_code} {r.text.strip()[:60]}")

# 6) password guessing
print(f"{BOLD}6. Password guessing on admin (6 tries){END}")
g = requests.Session(); g.verify = False
codes = []
for i in range(6):
    r = g.post(BASE + "/login", data={"username": "admin", "password": f"guess{i}"}, allow_redirects=False)
    codes.append("in" if r.status_code == 302 else ("locked" if "Too many" in r.text else "no"))
check("guess", "in" not in codes and codes[-1] == "locked", f"results: {codes}")

ok = sum(results)
color = GREEN if ok == len(results) else RED
print(f"{BOLD}{color}{ok}/{len(results)} attacks blocked.{END} See REJECTED / DENIED / LOGIN FAILED in the dashboard event log.")
print("Note: the ESP32 has NO open port; it only runs commands found in replies it verified with the key, with ids that only go up.")
