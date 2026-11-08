# TEAM PLANNING SHEET

| | | | |
|---|---|---|---|
| **Team name:** | Cappucin | **Property:** | 3 — Fire Detection (project: EMBER) |
| **Members:** | Israa Soudani, Mayssam Hergli, Moetez Zouari | **Team leader:** | Moetez Zouari |

**Constraint card:** `[TO FILL: write the card's rule + one line on how EMBER follows it]`

---

## THE PROBLEM IN OUR OWN WORDS

**Who is affected?** Families in homes that cook with butane bottles, small restaurants and workshops, and the building managers responsible for them.

**What can fail?** A small fire or gas leak starts at night or in an empty room and nobody sees it in time. Classic detectors give false alarms, so people ignore or unplug them. A detector that only beeps locally helps nobody when the room is empty.

**What does an incident cost?** Lives and injuries first, then the home or business, its stock and its income. Every minute of early warning gives people more time to leave and stops a small fire from becoming a big one.

---

## OUR SIX LAYERS

"Who is in charge" is left blank to be filled in by hand.

| Layer | What we will do | Who is in charge |
|---|---|---|
| **Sensing** | Three sensors of different kinds: gas sensor (smoke and combustible gas), DHT22 (temperature, used for how fast heat rises), LDR sampled fast (flame flicker). Plausibility checks flag a faulty or disconnected sensor: DHT22 returns no value, gas reading stuck at 0 or max, LDR flat. One odd reading never starts an alarm: decisions use a 5 s window. | |
| **Communication** | The ESP32 sends a reading packet every 0.5 s over Wi-Fi to the laptop gateway, and receives commands back. Traffic is protected with HTTPS (TLS, pinned certificate). Every command is also signed with HMAC-SHA256 plus a counter, so a copied or replayed command is rejected. | |
| **Edge processing and AI** | A logistic regression model runs on the ESP32. It reads a 5 s window (10 readings) and computes 6 features: gas above baseline, gas slope, heat slope, light change, flicker, camera flame score (optional, laptop OpenCV). Output: probability of normal, gas leak, fire, mapped to NORMAL / WARNING / ALARM. Trained on data we record, including traps (flashlight, warm hand). Tested on at least 20 labeled windows with a confusion matrix, accuracy, precision and recall. Key metric: fire recall. Compared to a simple threshold rule. | |
| **Actuation** | Automatic: LEDs green, yellow, red; buzzer with one pattern per level; servo as the gas shut-off valve, closed at gas leak or fire. Manual: an allowed user can open or close the valve and silence the buzzer from the dashboard. Safe state on failure or lost link: valve closed, local alarm keeps running. | |
| **Security** | Threat 1: someone resets or silences the alarm without the right to (prank, intruder). Protection: dashboard login, reset needs an authorized account, the board's button can only silence for 60 s, never reset. Threat 2: fake or replayed commands on the network (for example "open valve"). Protection: HMAC-SHA256 signature + counter, rejected and logged. Threat 3: eavesdropping or tampered data in transit. Protection: TLS. Threat 4: connection lost or jammed. Protection: the board decides and acts alone. | |
| **Operator interface** | Web dashboard on the laptop: live values, a big colored status banner (NORMAL / WARNING / ALARM), fire probability with "why" bars, sensor fault indicators, event history, commands (authorized reset, silence, valve open or close). A non-expert reads it in a few seconds. | |

---

## OUR ARCHITECTURE

```
              OPERATOR INTERFACE (web dashboard on the laptop)
   [T1] status banner, live values, fire probability
   [T4] login, authorized reset; event history, commands
                  ^                           |
     reading packet every 0.5 s      commands, HMAC + counter
                  |                           v
       COMMUNICATION: Wi-Fi to the laptop gateway, HTTPS (TLS, pinned cert)
                  ^                           |
==================|===========================|==================
 ESP32: senses, decides and acts WITHOUT the network
                                              v
 SENSING             EDGE AI                  ACTUATION
 Gas, DHT22, LDR --> 5 s window          -->  [T1] LEDs + buzzer
 Plausibility [T3]   Logistic regression      Servo gas valve
 Button: silence     Normal/leak/fire [T1]    Safe state: valve closed,
 only, 60 s [T4]     0.5 s loop [T2]          local alarm keeps running
                     Trained on traps [T3]
==================================================================
 SECURITY, all layers: dashboard login, signed commands
 (HMAC + counter), TLS, button can only silence, rejections logged
```

| Target | Where it is handled |
|---|---|
| **T1** Three danger levels | Model output on the ESP32, shown on the LEDs and the dashboard banner |
| **T2** Alarm within 20 s | 0.5 s loop on a 5 s window. Our target: under 10 s |
| **T3** No alarm in normal conditions | Model trained on traps (flashlight, warm hand) + 5 s window against odd readings |
| **T4** Only an allowed person resets | Dashboard login + signed reset command; the board button only silences for 60 s |

---

## OUR PARTS LIST

| Part | How many | Cost (credits) | Why we need it | Alternative if sold out |
|---|---|---|---|---|
| ESP32 development board | 1 | 150 | Brain, Wi-Fi, runs the AI model | Arduino Uno (100), model runs on the laptop over USB |
| Gas sensor | 1 | 90 | Smoke and combustible gas | None in the bank: DHT22 + LDR still give two sensors |
| DHT22 sensor | 1 | 70 | Temperature, how fast heat rises | Gas + LDR still give two sensors |
| LDR light sensor | 1 | 30 | Flame flicker | Infrared obstacle sensor (40) |
| Servo motor | 1 | 70 | Gas shut-off valve, closes by itself | Small fan (80) as a vent, gas leak only |
| Buzzer | 1 | 20 | Siren, one pattern per level | Dashboard siren + red LED strobe on the board |
| LED, red + yellow + green | 3 | 30 | Shows the danger level on the board | Fewer LEDs + level shown in the buzzer pattern |
| Push button + extra wires, male-female | 1 + 1 pack | 10 + 5 | Local silence for 60 s, never a reset; spare wiring | Dashboard silence only; starter pack wires only |

**Total cost:** 475 credits  **Our budget:** 490 credits  **Estimated unit cost of the real deployment:** about 160–260 DT per room (detector + gas valve; rough estimate; gateway, certification and installation extra)

---

## HOW OUR PROTOTYPE MAPS TO THE REAL SOLUTION

**What changes:** LDR → infrared flame sensor; hobby gas sensor → certified gas and smoke sensor; servo → gas solenoid valve that closes without power; laptop → building gateway + cloud dashboard; local Wi-Fi → building Wi-Fi or LoRa with TLS; one USB-powered room → one detector per room on mains + battery; safe test condition → real smoke, heat and flame.

**Added in the real product:** voice alerts that say where the danger is; interconnection, so all detectors in the building alarm together; building actions (gas closed, ventilation off, room power cut); automatic call to 198 with a spoken brief (room, floor, intensity, gas hazard).

**What is preserved:** the three early signs; the same 5 s model on the detector, three levels, leak vs fire; acts without internet, safe state; login, signed commands, encrypted traffic, authorized reset.

**Why it stays representative:** same decision logic, data flow and security; better sensors and valve, model retrained on real data.

---

## OUR BIGGEST RISK AND OUR PLAN B (ALSO FOR SOLD-OUT PARTS)

**Biggest risk:** the model meets conditions it was not trained on, giving a false or missed alarm in the demo: the organizers set the safe test condition and the gas sensor drifts until warm.

**Plan B:** ask for the test condition first and record training data on exactly that, plus traps (flashlight, warm hand); power the gas sensor 20+ minutes before recording or demo; the 5 s window rejects odd readings; rehearsed backup demo mode if hardware fails.

**Network failure:** the board decides and acts alone; phone hotspot for the link to the laptop if the venue Wi-Fi fails.

**Sold-out parts:** we ask first for the gas sensor (4 in stock) and the buzzer (3). Every part has an alternative above, within 490 credits.
