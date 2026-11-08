# EMBER — Team plan (IoT Spark 4.0, team Cappucin)

> Everyone reads this file. Your own brief (01 / 02 / 03) is what you give to your Claude.
> The **Shared contract** at the bottom is identical in all four files. Never change it without telling the whole team.

## What we are building (one sentence)
A room detector that senses gas, fast-rising heat and flame flicker, uses an AI model on the ESP32 (plus a camera model on a laptop) to decide NORMAL / WARNING / ALARM, sounds a clear alarm, closes the gas valve, and lets only an authorized person reset it.

## Scoring reminders (100 pts)
Pitch + demo 30 · Build 24 · Plan 13 · AI 12 · Security 10 · Dashboard 6 · Technical understanding 5.
- **Basics done well beat unfinished extras.** Always keep a working version saved.
- **AI standard:** model reads a window of readings, tested on ≥ 20 labeled windows, confusion matrix, accuracy / precision / recall, key metric explained, every member can explain it.
- **Targets:** 3 levels on board AND dashboard · alarm < 20 s · no false alarm during the whole demo · only an allowed person resets.
- **Jury will break something on purpose:** lost sensor, fake command, lost connection.
- **SAFETY: no real fire, flame or smoke.** Use only the test conditions the organizers allow.

## Roles
| Who | Owns | Main files |
|---|---|---|
| **Mayssam** | Hardware wiring, ESP32 firmware (sensors, features, model inference, levels, actuators, faults), data recording, training the sensor model + metrics | `ember_esp32/ember_esp32.ino`, `model.h`, `record_serial.py`, `train.py` |
| **Moetez** | Computer vision: flame score from the webcam, camera model + metrics, posting the score to the server | `cv_flame.py`, `cv_record.py`, `cv_train.py` |
| **Israa** | Flask server + dashboard, security (HMAC, TLS, login, roles, replay protection), ESP32 network module `ember_link.h`, fake/attack scripts, integration lead, slides + pitch | `server.py`, `templates/`, `ember_link.h`, `fake_esp32.py`, `attack_test.py` |

Shared folder (Drive or GitHub): one subfolder per person. `ember_link.h` goes from Israa to Mayssam.

## Timeline (now → pitch)
| Time | Mayssam | Moetez | Israa |
|---|---|---|---|
| **16:20–16:50** | Wire everything, each sensor printing on Serial, actuators tested. Gas sensor powered NOW (needs 20+ min warm-up). | Webcam + HSV mask + live flame score window. | Flask server: HMAC verify, `/api/reading`, `/api/cv`, `/api/state`; `fake_esp32.py`; generate `EMBER_KEY`, share it + `SERVER_IP`. |
| **16:50–17:10** | Features + fallback rules + levels + actuators + `DATA,` serial lines. **✅ Checkpoint A: board works alone with rules. Save a copy.** | Flicker + smoothing, annotated frame, POST to `/api/cv`. | `ember_link.h` (network task). **~17:05: 10-min slot on the board with Mayssam** to test ESP32 → server. |
| **17:10–17:35** | Record data (5 scenarios), `train.py`, metrics, paste `model.h`, flash. | Record labeled clips, `cv_train.py`, camera confusion matrix. | Dashboard: login + roles, commands, events, why bars, camera image, voice, comparisons; `attack_test.py`. |
| **17:35–17:50** | **Integration, all three:** run the checklist below. | | |
| **17:50** | **FREEZE 🧊** — no new features, only fixes. | Moetez: slides skeleton + CV slide. | Israa: slides, demo script, submit presentation link. |
| **18:00–18:20** | Rehearse the full demo twice (timed). Each member rehearses their Q&A answers. | | |

Whoever finishes early helps with slides. If something is late at a checkpoint: **cut it, don't extend.**

## Integration checklist (17:35)
1. ESP32 and both laptops on the hotspot. `SERVER_IP` correct everywhere. Same `EMBER_KEY` and `USE_TLS` everywhere.
2. Dashboard shows live values from the real board (not fake).
3. Camera score shows on the dashboard AND appears in the ESP32 packet (`cv`).
4. Dashboard **Reset / Silence / Valve** reach the board (watch `last_cmd` increase).
5. Failure tests (the jury will do these):
   - Unplug the DHT22 → dashboard shows **SENSOR FAULT dht**, no false alarm.
   - Stop `server.py` → board keeps working alone, valve closes (safe state), dashboard shows **BOARD OFFLINE** when it comes back.
   - Run `attack_test.py` → bad signature + replay + command without login are all **REJECTED** in the event log.
6. Run the demo script once end to end.

## Demo script (≈ 3 min inside the 5-min pitch)
1. **Normal:** green LED, dashboard NORMAL. "Nothing happens in normal conditions."
2. **Traps:** flashlight on the LDR → stays NORMAL. Warm hand on the DHT22 → stays NORMAL. Dashboard: *"simple threshold rule: WOULD ALARM — EMBER: normal"*.
3. **Gas test (organizer-approved condition)** → WARNING gas leak: yellow blink, beep, valve closes, voice message. Still there after 10 s → ALARM.
4. **Fire test (organizer-approved condition + phone flame video at the LDR and camera)** → ALARM fire, red strobe, siren, *"VISUALLY CONFIRMED"*.
5. **Reset:** button only silences 60 s. Viewer account → reset DENIED. Admin → reset → logged.
6. **Resilience (if time):** unplug a sensor → FAULT; attack script → REJECTED.

Pitch shape (5 min): problem 30 s → EMBER 30 s → live demo ~3 min → AI metrics slide 45 s → real life + cost + roadmap 30 s. Q&A: each person answers for their own part.

## Open decisions (decide by 16:30)
- [ ] **Safe test conditions** allowed by organizers for gas and for fire (ask now). Training data must use exactly these.
- [ ] **LDR or IR obstacle sensor?** If no LDR: light features are forced to 0 (no code change), IR becomes anti-masking ("detector covered").
- [ ] **Buzzer type:** active (on/off only) or passive (tones)?
- [ ] **TLS:** keep HTTPS (`USE_TLS=true`) unless it breaks by 17:10 → switch everyone to HTTP + HMAC.
- [ ] Hotspot name/password, `SERVER_IP`, `EMBER_KEY` shared in the team chat (never in slides).

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
