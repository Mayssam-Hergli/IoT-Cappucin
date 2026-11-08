"""Fake EMBER board: sends signed packets every 500 ms (sim: true), runs the
contract's decision logic, and executes signed commands from the server.
Keys: n normal | l gas leak | f fire | t trap (flashlight + warm hand) | d DHT fault toggle
      q stop/resume sending (tests BOARD OFFLINE) | x exit
Run:  python fake_esp32.py [server_url]
"""
import hashlib, hmac, json, random, sys, threading, time

import requests, urllib3

from config import BASE_URL, EMBER_KEY

urllib3.disable_warnings()
URL = (sys.argv[1] if len(sys.argv) > 1 else BASE_URL).rstrip("/") + "/api/reading"
LEVELS = ["NORMAL", "WARNING", "ALARM"]

def sign(p):
    return hmac.new(EMBER_KEY.encode(), p.encode(), hashlib.sha256).hexdigest()

class Board:
    def __init__(self):
        self.boot = random.randint(1, 2**31 - 1)
        self.seq = 0
        self.t0 = time.time()
        self.scenario = "normal"
        self.scen_t = time.time()
        self.dht_fault = False
        self.paused = False
        self.level, self.cause = 0, "none"
        self.up_count = self.calm_count = 0
        self.warn_leak_t = None
        self.latched = False
        self.silence_until = 0
        self.valve = "open"
        self.last_cmd = 0
        self.cv, self.cv_fresh = 0.0, False
        self.link_t = 0.0
        self.base_gas = 1700

    # ---- simulated sensors / features / model
    def sense(self):
        s, dt = self.scenario, time.time() - self.scen_t
        n = lambda a: random.uniform(-a, a)
        gas, temp, hum, ldr = self.base_gas + n(15), 24.5 + n(0.1), 41 + n(0.5), 2100 + n(20)
        feat = {"gas_delta": n(10), "gas_slope": n(2), "temp_slope": n(0.02),
                "light_delta": n(20), "flicker": abs(n(3))}
        probs = {"normal": 0.96, "leak": 0.02, "fire": 0.02}
        if s == "leak":
            rise = min(900, dt * 120)
            gas += rise
            feat.update(gas_delta=rise, gas_slope=120 if rise < 900 else 5)
            pl = min(0.9, 0.1 + dt * 0.15)
            probs = {"normal": 1 - pl - 0.02, "leak": pl, "fire": 0.02}
        elif s == "fire":
            temp += min(15, dt * 1.5)
            ldr -= 600
            feat.update(temp_slope=1.5, light_delta=-600 + n(100), flicker=60 + n(10),
                        gas_delta=80 + n(20))
            pf = min(0.85, 0.15 + dt * 0.12)
            probs = {"normal": 1 - pf - 0.03, "leak": 0.03, "fire": pf}
        elif s == "trap":   # flashlight on LDR + warm hand: rule alarms, model stays calm
            temp += 4
            ldr += 1500
            feat.update(light_delta=1500 + n(50), temp_slope=0.6, flicker=5)
            probs = {"normal": 0.88, "leak": 0.02, "fire": 0.10}
        faults = []
        if self.dht_fault:
            temp = hum = None
            feat["temp_slope"] = 0.0
            faults.append("dht")
        # camera late fusion
        if self.cv_fresh:
            pf = 1 - (1 - probs["fire"]) * (1 - 0.5 * self.cv)
            probs["normal"] = max(0.0, probs["normal"] - (pf - probs["fire"]))
            probs["fire"] = pf
        raw = {"gas": int(gas), "temp": None if temp is None else round(temp, 1),
               "hum": None if hum is None else round(hum, 1), "ldr": int(ldr), "ir": 1}
        return raw, {k: round(v, 2) for k, v in feat.items()}, {k: round(v, 3) for k, v in probs.items()}, faults

    def decide(self, probs):
        if probs["fire"] >= 0.6:
            cand, cause = 2, "fire"
        elif probs["leak"] >= 0.5:
            cand, cause = 1, "gas_leak"
        elif probs["fire"] >= 0.3:
            cand, cause = 1, "fire"
        else:
            cand, cause = 0, "none"
        if self.latched:
            return
        if cand > self.level:
            self.up_count += 1
            self.calm_count = 0
            if self.up_count >= 2:
                self.level, self.cause, self.up_count = cand, cause, 0
        else:
            self.up_count = 0
            if cand < self.level:
                self.calm_count += 1
                if self.calm_count >= 20:
                    self.level, self.cause, self.calm_count = cand, cause, 0
            else:
                self.calm_count = 0
        if self.level == 1 and self.cause == "gas_leak":
            self.warn_leak_t = self.warn_leak_t or time.time()
            if time.time() - self.warn_leak_t >= 10:
                self.level = 2
        else:
            self.warn_leak_t = None
        if self.level == 2:
            self.latched = True
        if self.cause == "gas_leak" or self.level == 2:
            self.valve = "closed"

    def link_ok(self):
        return time.time() - self.link_t < 3

    def payload(self):
        raw, feat, probs, faults = self.sense()
        if not self.link_ok():
            self.cv, self.cv_fresh = 0.0, False
            if self.link_t:
                self.valve = "closed"      # safe state when link lost
        self.decide(probs)
        self.seq += 1
        cls = "leak" if self.cause == "gas_leak" else "fire"
        why = {k: round(min(1.0, abs(v) / {"gas_delta": 900, "gas_slope": 150, "temp_slope": 2,
                                           "light_delta": 1500, "flicker": 80}[k]) * probs[cls], 2)
               for k, v in feat.items()}
        return {"dev": "room1", "boot": self.boot, "seq": self.seq,
                "up_ms": int((time.time() - self.t0) * 1000), "sim": True,
                "raw": raw, "feat": feat, "cv": round(self.cv if self.cv_fresh else 0.0, 3),
                "probs": probs, "why": why, "level": self.level, "state": LEVELS[self.level],
                "cause": self.cause, "mode": "model", "latched": self.latched,
                "silenced": time.time() < self.silence_until, "valve": self.valve,
                "faults": faults, "tamper": False, "link_ok": self.link_ok(),
                "last_cmd": self.last_cmd}

    def run_cmd(self, cid, cmd):
        print(f"\n>>> EXEC command id={cid} {cmd}")
        if cmd == "reset":
            self.latched, self.level, self.cause = False, 0, "none"
            self.warn_leak_t = None
            self.scenario, self.scen_t = "normal", time.time()
        elif cmd == "silence":
            self.silence_until = time.time() + 60
        elif cmd == "valve_open":
            self.valve = "open"
        elif cmd == "valve_close":
            self.valve = "closed"
        self.last_cmd = cid

board = Board()
http = requests.Session()

def sender():
    while True:
        time.sleep(0.5)
        if board.paused:
            continue
        p = json.dumps(board.payload(), separators=(",", ":"))
        try:
            r = http.post(URL, json={"p": p, "sig": sign(p)}, timeout=1.5, verify=False)
            if r.status_code != 200:
                print(f"\n[server] HTTP {r.status_code} {r.text[:80]}")
                continue
            env = r.json()
            if not hmac.compare_digest(sign(env.get("p", "")), env.get("sig", "")):
                print("\n[link] reply signature INVALID, ignored")
                continue
            rep = json.loads(env["p"])
            board.link_t = time.time()
            board.cv, board.cv_fresh = float(rep.get("cv", 0)), bool(rep.get("cv_fresh"))
            for c in rep.get("cmds", []):
                if c["id"] > board.last_cmd:
                    board.run_cmd(c["id"], c["cmd"])
        except Exception as e:
            print(f"\n[link] {type(e).__name__}: {e}")
        sys.stdout.write(f"\r{board.scenario:7s} {LEVELS[board.level]:7s} cause={board.cause:8s} "
                         f"valve={board.valve:6s} seq={board.seq} link={'OK' if board.link_ok() else '--'} "
                         f"cv={board.cv:.2f}{'*' if board.cv_fresh else ' '} last_cmd={board.last_cmd}   ")
        sys.stdout.flush()

def set_scen(s):
    board.scenario, board.scen_t = s, time.time()
    print(f"\n=== scenario: {s}")

def main():
    print(__doc__)
    print("Posting to", URL, "boot", board.boot)
    threading.Thread(target=sender, daemon=True).start()
    try:
        import msvcrt
        getkey = lambda: msvcrt.getwch().lower()
    except ImportError:
        getkey = lambda: (sys.stdin.readline().strip() or " ")[0].lower()
    while True:
        k = getkey()
        if k == "n": set_scen("normal")
        elif k == "l": set_scen("leak")
        elif k == "f": set_scen("fire")
        elif k == "t": set_scen("trap")
        elif k == "d":
            board.dht_fault = not board.dht_fault
            print(f"\n=== DHT fault {'ON' if board.dht_fault else 'OFF'}")
        elif k == "q":
            board.paused = not board.paused
            print(f"\n=== sending {'STOPPED' if board.paused else 'RESUMED'}")
        elif k == "x":
            break

if __name__ == "__main__":
    main()
