# Brief for Claude — Moetez: computer vision (flame detection)

You are helping **Moetez** at a hackathon (IoT Spark 4.0, ~2 h left, freeze at 17:50). Project **EMBER**: early fire and gas-leak detection. Moetez owns the **camera layer**: a Python program on his laptop that watches the webcam, computes a **flame score 0..1**, and sends it with an annotated frame to Israa's server. The server forwards the score to the ESP32, which fuses it with the sensor model. Be fast and concrete: complete runnable code, short explanations, test steps.

**SAFETY: no real fire, flame or smoke at the event.** The flame is simulated with a **fire video played on a phone** held in front of the webcam. Real-world equivalent (for the pitch): a camera or IR flame sensor seeing a real flame.

Teammates: **Mayssam** (ESP32 + sensor model), **Israa** (server, dashboard, security). The **Shared contract** at the end is fixed; follow it exactly.

## Design rules
- The camera can only push EMBER to **WARNING alone**; ALARM needs sensors to agree (fusion is on the ESP32). So the camera should favour **precision** (few false "flame" scores).
- Keep it light: **OpenCV + numpy** only for the core. No PyTorch unless it is already installed and everything else works.
- Must run ≥ 8 fps on the laptop CPU at 320×240.
- If the server is down, keep running locally (show the window), retry quietly.

## Install
```
pip install opencv-python numpy requests scikit-learn matplotlib
```

## Level 1 (must, by 16:50): flame score from colour + flicker — `cv_flame.py`
Per frame (resize to 320×240):
1. **Colour mask** (tune live with trackbars):
```python
hsv = cv2.cvtColor(frame, cv2.COLOR_BGR2HSV)
m1 = cv2.inRange(hsv, (0, 80, 180), (35, 255, 255))     # red → orange → yellow, bright
m2 = cv2.inRange(hsv, (170, 80, 180), (180, 255, 255))  # red wrap-around
b, g, r = cv2.split(frame)
rgb_rule = ((r > 180) & (r >= g) & (g > b)).astype(np.uint8) * 255   # classic fire colour rule
mask = cv2.bitwise_and(cv2.bitwise_or(m1, m2), rgb_rule)
```
   Then morphological open/close, contours, ignore blobs < 0.2 % of the frame.
2. **Features:**
   - `area_frac` = mask pixels / frame pixels
   - `flicker` = mean |gray_t − gray_(t−1)| inside the dilated mask / 255
   - `area_var` = std / mean of `area_frac` over the last 10 frames
   - `bright` = mean V inside the mask / 255
3. **Score:** `s = clip(area_frac / 0.02, 0, 1) × clip(flicker / 0.08 + area_var / 0.3, 0, 1)`, then EMA (α = 0.3). `flame = score ≥ 0.5` for 3 of the last 5 frames. A static orange object has ~0 flicker → low score; a white flashlight fails the colour rule.
4. Draw boxes + score on the frame, show it in a window.
5. If the camera's auto-exposure jumps, try locking it (`cv2.CAP_PROP_AUTO_EXPOSURE`; behaviour depends on the OS/driver).

## Level 1b (by 17:10): send to the server
- A background thread every ~300 ms POSTs to `/api/cv` (contract section 2): JPEG of the annotated frame (quality 60, 320×240) → base64.
- Envelope with HMAC-SHA256 over the exact payload string:
```python
import hmac, hashlib, json
p = json.dumps(payload, separators=(",", ":"))
sig = hmac.new(EMBER_KEY.encode(), p.encode(), hashlib.sha256).hexdigest()
session.post(f"{BASE}/api/cv", json={"p": p, "sig": sig}, timeout=1, verify=False)
```
  `BASE` = `https://SERVER_IP:5443` (or `http://SERVER_IP:5000` if `USE_TLS=false`). `verify=False` because the certificate is self-signed; silence the warning with `urllib3.disable_warnings()`. Put `EMBER_KEY`, `SERVER_IP`, `USE_TLS` at the top of the file.
- Test with Israa: your score and frame appear on the dashboard.

## Level 2 (should, by 17:35): a real camera model with metrics — `cv_record.py` + `cv_train.py`
This gives the camera its own confusion matrix (strong for the AI score).
1. `cv_record.py`: shows the camera; hold key **f** while recording flame clips (phone fire video at different distances and angles), **n** for no-flame clips. Make the no-flame clips hard: empty scene, people moving, red or orange objects, a phone showing a bright non-fire video, the flashlight. Each key press starts a new **clip id**.
2. Features per **1-second window** (10 frames): mean and max of `area_frac`, mean `flicker`, `area_var`, mean `bright`. Save rows: clip_id, label, features.
3. `cv_train.py`: **split by clip, not by frame** (frames of one clip are near copies, so a frame split would leak). `StandardScaler` + `LogisticRegression`. Print the confusion matrix (save a PNG), accuracy, precision, recall. Key metric for the camera: **precision**. Need ≥ 20 test windows.
4. Compare with the Level 1 hand-tuned score on the same test windows.
5. If better, use the model's probability as `score` in `cv_flame.py` (save it with `joblib`).

## Level 3 (only if Levels 1 and 2 are done and installed by 17:15): pretrained detector
A pretrained YOLO fire/smoke model (search Hugging Face for one; check its licence). Only if `ultralytics` is already installed or installs fast, and it runs ≥ 5 fps on CPU. Otherwise skip and mention it as roadmap.

## Checkpoints
- **16:50** live window with mask + score; phone fire video → high score; orange object, flashlight → low.
- **17:10** score + frame visible on Israa's dashboard.
- **17:35** camera model metrics + confusion matrix PNG sent to Israa.
- **17:50 freeze** → you own the slides skeleton + the camera slide.

## Be ready to explain (Q&A)
What the camera looks for (fire colours **and** flicker; flames move at a few Hz, objects don't), why the camera alone can't trigger ALARM (a fire on a TV screen should not evacuate a building), the windowed features and the split by clip, the confusion matrix and why precision matters most here, limits (lighting, distance, camera angle, orange moving objects), privacy (processed locally, only a score and a small frame are sent; the real product would process on the device and never upload video).

---

## Shared contract (identical in all four files — do not change without telling the team)

### Network
- All devices on one **phone hotspot, 2.4 GHz** (the ESP32 cannot join 5 GHz). SSID `EMBER-NET`, password: *fill in*.
- Server = Israa's laptop. `SERVER_IP` = its IP on the hotspot (`ipconfig`). Allow Python through the Windows firewall (Private networks).
- `USE_TLS = true` → `https://SERVER_IP:5443` (self-signed certificate; clients skip certificate verification because the HMAC proves who sent each message). Fallback `USE_TLS = false` → `http://SERVER_IP:5000`. Everyone uses the same setting.
- Shared secret `EMBER_KEY` = 32 hex characters generated by Israa: `python -c "import secrets;print(secrets.token_hex(16))"`. Never in slides or screenshots.

### Envelope (every message, both directions)
```json
{"p": "<payload serialized as a JSON string>", "sig": "<lowercase hex HMAC-SHA256(EMBER_KEY, p)>"}
```
- The signature covers the **exact bytes of the string `p`**. The receiver verifies first (constant-time compare), then parses `p`.
- Bad signature → ignored, logged as REJECTED.

### 1) ESP32 → server: `POST /api/reading` every 500 ms
Payload `p`:
```json
{
  "dev": "room1", "boot": 48213, "seq": 42, "up_ms": 21000, "sim": false,
  "raw":  {"gas": 1830, "temp": 24.6, "hum": 41.0, "ldr": 2100, "ir": 1},
  "feat": {"gas_delta": 120.0, "gas_slope": 15.2, "temp_slope": 0.02, "light_delta": -30.0, "flicker": 4.1},
  "cv": 0.0,
  "probs": {"normal": 0.97, "leak": 0.02, "fire": 0.01},
  "why":  {"gas_delta": 0.4, "gas_slope": 0.1, "temp_slope": 0.0, "light_delta": 0.0, "flicker": 0.0},
  "level": 0, "state": "NORMAL", "cause": "none",
  "mode": "model", "latched": false, "silenced": false, "valve": "open",
  "faults": [], "tamper": false, "link_ok": true, "last_cmd": 0
}
```
- `level`: 0 = NORMAL, 1 = WARNING, 2 = ALARM. `state`: same as text. `cause`: `"none"` | `"gas_leak"` | `"fire"`.
- `probs.fire` is the value **after** camera fusion. `why` = contribution of each feature to the class behind the current cause (fire if none).
- `mode`: `"model"` (logistic regression) or `"rules"` (fallback). `faults`: any of `"gas"`, `"dht"`, `"ldr"`. `tamper`: IR sensor covered > 5 s (if fitted).
- `boot` = random number picked at each ESP32 boot. `seq` +1 each packet. Server rejects `seq` ≤ last seq of the same boot (replay).
- `last_cmd` = id of the last command the board executed (works as the ACK).
- `sim: true` only from `fake_esp32.py`; the dashboard then shows a SIMULATION banner.

Server reply (HTTP 200), same envelope, payload:
```json
{"ok": true, "cv": 0.82, "cv_fresh": true, "cmds": [{"id": 941234567, "cmd": "reset"}]}
```
- `cmd` ∈ `"reset"` | `"silence"` | `"valve_open"` | `"valve_close"`.
- `id` = `max(last_id + 1, int((time.time() - 1_700_000_000) * 10))`: always increasing, fits in 32 bits. The board runs a command only if the signature is valid **and** `id` > last executed id. The server re-sends a command until the board's `last_cmd` ≥ its id.
- `cv` = latest camera flame score 0..1. `cv_fresh` = camera posted less than 2 s ago.
- 401 = bad signature. 409 = replayed seq.

### 2) Camera → server: `POST /api/cv` every ~300 ms
Payload `p`:
```json
{"src": "cam1", "seq": 101, "score": 0.82, "flame": true, "boxes": [[120, 80, 60, 90]], "fps": 14.2, "jpg": "<base64 JPEG 320x240, annotated>"}
```
Reply: `{"ok": true}` (plain JSON).

### 3) Decision logic (runs on the ESP32)
- Sensor model: 5 features → logistic regression → p_normal, p_leak, p_fire_sensor.
- Camera late fusion: if `cv_fresh`, `p_fire = 1 − (1 − p_fire_sensor) × (1 − 0.5 × cv)`, else `p_fire = p_fire_sensor`. The camera alone can reach WARNING, never ALARM.
- Candidate level: **ALARM** if p_fire ≥ 0.6 · **WARNING** if p_leak ≥ 0.5 or p_fire ≥ 0.3 · else **NORMAL**.
- Go up after 2 ticks in a row (1 s). Go down from WARNING after 20 calm ticks (10 s). **ALARM stays latched until an authorized `reset`.**
- A gas leak that stays WARNING for 10 s escalates to ALARM (cause `gas_leak`).
- Response budget: window fills in ≤ 5 s + 1 s confirmation → alarm in under 10 s (target ≤ 20 s).
- Link lost (no valid reply for 3 s): board keeps deciding alone, `cv` = 0, valve closes (safe state).
