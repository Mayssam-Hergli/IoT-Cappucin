"""EMBER server: signed ESP32/camera ingest, dashboard, commands, security events.
Run:  python server.py
"""
import base64, csv, hashlib, hmac, json, os, secrets, threading, time
from functools import wraps

from flask import (Flask, Response, abort, jsonify, redirect, render_template,
                   request, session, url_for)
from werkzeug.security import check_password_hash, generate_password_hash
from werkzeug.serving import WSGIRequestHandler

# keep-alive so the ESP32 reuses its TLS connection (one handshake, not one per packet)
WSGIRequestHandler.protocol_version = "HTTP/1.1"

import config
from config import EMBER_KEY, USE_TLS, PORT

app = Flask(__name__)
app.secret_key = secrets.token_hex(32)
app.config.update(SESSION_COOKIE_SAMESITE="Strict", SESSION_COOKIE_HTTPONLY=True,
                  SESSION_COOKIE_SECURE=USE_TLS)

USERS = {u: {"hash": generate_password_hash(v["password"]), "role": v["role"]}
         for u, v in config.USERS.items()}
VALID_CMDS = {"reset", "silence", "valve_open", "valve_close",
              # simulated sensors (no DHT / LDR fitted): the board fakes these channels on request
              "sim_heat", "sim_flame", "sim_hand", "sim_torch", "sim_calm", "sim_dht_fault"}
ONLINE_S = 3.0
CV_FRESH_S = 2.0
HERE = os.path.dirname(os.path.abspath(__file__))
EVENTS_CSV = os.path.join(HERE, "events.csv")
READINGS_CSV = os.path.join(HERE, "readings.csv")

# ---------------------------------------------------------------- envelope
def sign(p: str) -> str:
    return hmac.new(EMBER_KEY.encode(), p.encode(), hashlib.sha256).hexdigest()

def open_envelope(env):
    """Return payload dict if the signature is valid, else None."""
    if not isinstance(env, dict):
        return None
    p, sig = env.get("p", ""), env.get("sig", "")
    if not isinstance(p, str) or not isinstance(sig, str):
        return None
    if not hmac.compare_digest(sign(p), sig.lower()):
        return None
    try:
        obj = json.loads(p)
    except ValueError:
        return None
    return obj if isinstance(obj, dict) else None

def make_envelope(obj):
    p = json.dumps(obj, separators=(",", ":"))
    return {"p": p, "sig": sign(p)}

# ---------------------------------------------------------------- state
lock = threading.RLock()
S = {
    "reading": None, "reading_t": 0.0, "online": False, "ever_online": False,
    "last_seq": {},              # (dev, boot) -> last seq
    "cmds": [], "last_id": 0,    # pending commands
    "cv": {"score": 0.0, "flame": False, "boxes": [], "fps": 0.0, "t": 0.0, "src": None},
    "jpg": None,
    "events": [],
    "alert_t": None,             # time EMBER first left NORMAL in the current incident
}
LOGIN_FAILS = {}                 # ip -> (count, lock_until)

def event(typ, detail=""):
    t = time.time()
    with lock:
        S["events"].append({"t": t, "type": typ, "detail": detail})
        del S["events"][:-500]
    try:
        with open(EVENTS_CSV, "a", newline="", encoding="utf-8") as f:
            csv.writer(f).writerow([time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t)), typ, detail])
    except OSError:
        pass
    print(f"[EVENT] {typ} {detail}")

READ_COLS = ["t", "dev", "boot", "seq", "sim", "gas", "temp", "hum", "ldr", "ir",
             "gas_delta", "gas_slope", "temp_slope", "light_delta", "flicker",
             "cv", "p_normal", "p_leak", "p_fire", "level", "state", "cause", "mode"]

def log_reading(r):
    raw, feat, probs = r.get("raw", {}), r.get("feat", {}), r.get("probs", {})
    row = [round(time.time(), 2), r.get("dev"), r.get("boot"), r.get("seq"), r.get("sim"),
           raw.get("gas"), raw.get("temp"), raw.get("hum"), raw.get("ldr"), raw.get("ir"),
           feat.get("gas_delta"), feat.get("gas_slope"), feat.get("temp_slope"),
           feat.get("light_delta"), feat.get("flicker"), r.get("cv"),
           probs.get("normal"), probs.get("leak"), probs.get("fire"),
           r.get("level"), r.get("state"), r.get("cause"), r.get("mode")]
    try:
        new = not os.path.exists(READINGS_CSV)
        with open(READINGS_CSV, "a", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            if new:
                w.writerow(READ_COLS)
            w.writerow(row)
    except OSError:
        pass

def diff_events(old, new):
    """Create events for meaningful changes between two readings."""
    if old is None or old.get("boot") != new.get("boot"):
        event("BOARD BOOT", f"dev={new.get('dev')} boot={new.get('boot')} mode={new.get('mode')}"
                            + (" SIMULATION" if new.get("sim") else ""))
        old = old or {}
    if old.get("state") != new.get("state") or old.get("cause") != new.get("cause"):
        cause = new.get("cause", "none")
        event(f"STATE {new.get('state')}", "" if cause == "none" else f"cause={cause}")
    of, nf = set(old.get("faults") or []), set(new.get("faults") or [])
    for f in sorted(nf - of):
        event("SENSOR FAULT", f)
    for f in sorted(of - nf):
        event("SENSOR OK", f)
    if bool(old.get("tamper")) != bool(new.get("tamper")):
        event("TAMPER" if new.get("tamper") else "TAMPER CLEARED",
              "detector covered" if new.get("tamper") else "")
    for key, label in (("valve", "VALVE"), ("mode", "MODE")):
        if key in old and old.get(key) != new.get(key):
            event(f"{label} {str(new.get(key)).upper()}")
    if bool(old.get("latched")) != bool(new.get("latched")) and "latched" in old:
        event("LATCHED" if new.get("latched") else "UNLATCHED")
    if bool(old.get("silenced")) != bool(new.get("silenced")) and "silenced" in old:
        event("SILENCED" if new.get("silenced") else "SILENCE ENDED")
    if (new.get("last_cmd") or 0) != (old.get("last_cmd") or 0) and "last_cmd" in old:
        event("COMMAND ACK", f"board executed id={new.get('last_cmd')}")

# ---------------------------------------------------------------- auth helpers
def login_required(api=False):
    def deco(fn):
        @wraps(fn)
        def wrapper(*a, **kw):
            if "user" not in session:
                if api:
                    return jsonify({"ok": False, "error": "login required"}), 401
                return redirect(url_for("login"))
            return fn(*a, **kw)
        return wrapper
    return deco

# ---------------------------------------------------------------- device API
@app.post("/api/reading")
def api_reading():
    env = request.get_json(force=True, silent=True)
    r = open_envelope(env)
    ip = request.remote_addr
    if r is None:
        event("REJECTED bad signature", f"/api/reading from {ip}")
        return jsonify({"ok": False, "error": "bad signature"}), 401
    dev, boot, seq = r.get("dev", "?"), r.get("boot"), r.get("seq")
    if not isinstance(seq, int) or boot is None:
        event("REJECTED malformed", f"/api/reading from {ip}")
        return jsonify({"ok": False, "error": "missing boot/seq"}), 400
    now = time.time()
    with lock:
        key = (dev, boot)
        last = S["last_seq"].get(key)
        if last is not None and seq <= last:
            event("REJECTED replay", f"dev={dev} boot={boot} seq={seq} <= {last} from {ip}")
            return jsonify({"ok": False, "error": "replay"}), 409
        S["last_seq"][key] = seq

        old = S["reading"]
        diff_events(old, r)
        # incident timer for "EMBER alerted at"
        lvl = r.get("level", 0) or 0
        if lvl > 0 and S["alert_t"] is None:
            S["alert_t"] = now
        elif lvl == 0:
            S["alert_t"] = None
        S["reading"], S["reading_t"] = r, now
        if not S["online"]:
            if S["ever_online"]:
                event("BOARD BACK ONLINE", f"dev={dev}")
            S["online"] = S["ever_online"] = True

        ack = r.get("last_cmd") or 0
        S["cmds"] = [c for c in S["cmds"] if c["id"] > ack]
        cv = S["cv"]
        reply = {"ok": True, "cv": round(float(cv["score"]), 3),
                 "cv_fresh": (now - cv["t"]) < CV_FRESH_S, "cmds": list(S["cmds"])}
    log_reading(r)
    return jsonify(make_envelope(reply))

@app.post("/api/cv")
def api_cv():
    env = request.get_json(force=True, silent=True)
    c = open_envelope(env)
    if c is None:
        event("REJECTED bad signature", f"/api/cv from {request.remote_addr}")
        return jsonify({"ok": False, "error": "bad signature"}), 401
    jpg = None
    if c.get("jpg"):
        try:
            jpg = base64.b64decode(c["jpg"])
        except (ValueError, TypeError):
            jpg = None
    with lock:
        was_fresh = (time.time() - S["cv"]["t"]) < CV_FRESH_S
        S["cv"] = {"score": float(c.get("score", 0) or 0), "flame": bool(c.get("flame")),
                   "boxes": c.get("boxes") or [], "fps": float(c.get("fps", 0) or 0),
                   "t": time.time(), "src": c.get("src")}
        if jpg:
            S["jpg"] = jpg
    if not was_fresh:
        event("CAMERA ONLINE", str(c.get("src")))
    return jsonify({"ok": True})

# ---------------------------------------------------------------- browser
@app.route("/login", methods=["GET", "POST"])
def login():
    error = None
    if request.method == "POST":
        ip = request.remote_addr
        cnt, until = LOGIN_FAILS.get(ip, (0, 0))
        if time.time() < until:
            error = "Too many attempts. Wait 30 s."
        else:
            u = request.form.get("username", "")
            pw = request.form.get("password", "")
            user = USERS.get(u)
            if user and check_password_hash(user["hash"], pw):
                session.clear()
                session["user"], session["role"] = u, user["role"]
                LOGIN_FAILS.pop(ip, None)
                event("LOGIN", f"{u} ({user['role']}) from {ip}")
                return redirect(url_for("index"))
            cnt += 1
            LOGIN_FAILS[ip] = (cnt, time.time() + 30 if cnt >= 5 else 0)
            event("LOGIN FAILED", f"user={u[:32]!r} from {ip}")
            error = "Wrong username or password."
    return render_template("login.html", error=error)

@app.get("/logout")
def logout():
    if "user" in session:
        event("LOGOUT", session["user"])
    session.clear()
    return redirect(url_for("login"))

@app.get("/")
@login_required()
def index():
    return render_template("index.html", user=session["user"], role=session["role"])

@app.get("/cv.jpg")
@login_required(api=True)
def cv_jpg():
    with lock:
        jpg = S["jpg"]
    if not jpg:
        abort(404)
    return Response(jpg, mimetype="image/jpeg", headers={"Cache-Control": "no-store"})

def comparisons(r, alert_t):
    if not r:
        return None
    feat, raw = r.get("feat") or {}, r.get("raw") or {}
    reasons = []
    if (feat.get("gas_delta") or 0) >= config.RULE_GAS_DELTA:
        reasons.append("gas")
    if (feat.get("temp_slope") or 0) >= config.RULE_TEMP_SLOPE:
        reasons.append("heat rise")
    if abs(feat.get("light_delta") or 0) >= config.RULE_LIGHT_DELTA:
        reasons.append("light")
    temp = raw.get("temp")
    return {"rule_alarm": bool(reasons), "rule_reasons": reasons,
            "heat_alarm": temp is not None and temp >= config.HEAT_DETECTOR_C,
            "temp": temp, "alert_t": alert_t}

@app.get("/api/state")
@login_required(api=True)
def api_state():
    now = time.time()
    with lock:
        cv = dict(S["cv"])
        cv["fresh"] = (now - cv["t"]) < CV_FRESH_S
        cv["age"] = round(now - cv["t"], 1) if cv["t"] else None
        cv["has_jpg"] = S["jpg"] is not None
        return jsonify({
            "now": now,
            "reading": S["reading"],
            "age": round(now - S["reading_t"], 1) if S["reading_t"] else None,
            "online": S["online"],
            "cv": cv,
            "events": S["events"][-50:][::-1],
            "pending": list(S["cmds"]),
            "user": session["user"], "role": session["role"],
            "compare": comparisons(S["reading"], S["alert_t"]),
        })

@app.post("/api/command")
@login_required(api=True)
def api_command():
    body = request.get_json(force=True, silent=True) or {}
    cmd = body.get("cmd")
    if cmd not in VALID_CMDS:
        return jsonify({"ok": False, "error": "unknown command"}), 400
    user, role = session["user"], session["role"]
    if role != "admin":
        event("DENIED", f"{cmd} by {user} ({role})")
        return jsonify({"ok": False, "error": "not allowed for role " + role}), 403
    with lock:
        cid = max(S["last_id"] + 1, int((time.time() - 1_700_000_000) * 10))
        S["last_id"] = cid
        S["cmds"].append({"id": cid, "cmd": cmd})
    event("COMMAND", f"{cmd} id={cid} by {user}")
    return jsonify({"ok": True, "id": cid})

@app.after_request
def headers(resp):
    resp.headers["X-Content-Type-Options"] = "nosniff"
    resp.headers["X-Frame-Options"] = "DENY"
    return resp

# ---------------------------------------------------------------- offline watcher
def watcher():
    while True:
        time.sleep(0.5)
        with lock:
            if S["online"] and time.time() - S["reading_t"] > ONLINE_S:
                S["online"] = False
                dev = (S["reading"] or {}).get("dev")
                event("BOARD OFFLINE", f"dev={dev} no reading for {ONLINE_S:.0f} s")

if __name__ == "__main__":
    threading.Thread(target=watcher, daemon=True).start()
    event("SERVER START", f"{'https' if USE_TLS else 'http'} port {PORT}")
    app.run(host="0.0.0.0", port=PORT, threaded=True,
            ssl_context="adhoc" if USE_TLS else None)
