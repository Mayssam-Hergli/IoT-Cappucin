# CONTEXT — IoT Spark 4.0, Property 3: Fire Detection (EMBER)

Living memory for the hackathon. Update it whenever refined context arrives.
Filled form: `spec/FIRE DETECTION Filled.pdf` (text version: `TEAM_PLANNING_SHEET.md`). Sources converted to Markdown in the folders next to each PDF (`spec/`, `briefs/`).

**Team:** Cappucin. Members: Israa Soudani, Mayssam Hergli, Moetez Zouari. Leader: Moetez Zouari.

## Open items (fill these first)

- [x] Team name, members, team leader
- [ ] Constraint Card rule, and how the plan follows it (blank in the PDF; the sheet must be complete for approval)
- [ ] "Who is in charge" column: left blank in the PDF on purpose, Moetez fills it in by hand
- [ ] **Organizers' safe test condition** (train on exactly this)
- [ ] Is a hair dryer allowed? It is mains powered and the rules say low voltage only. Fallback nuisance tests: warm hand, warm cup, breath
- [ ] Is unlit lighter gas allowed as a gas test?
- [ ] Parts actually bought (adjust BOM and pins if different)
- [ ] ESP32 and laptop can share the venue Wi-Fi, otherwise phone hotspot
- [ ] Verify the unit cost before the pitch (figures come from Brief A and are unverified)
- [ ] What happens if a Chance card charges 20 credits when we have spent all 490

## Event rules

Event: IoT Spark 4.0, IEEE IAS/IES/PES ESPRIT SBJC. Teams of 2 or 3. Property 3 of 7.

**Problem:** most fires start small; nobody sees them in time; early detection gives time to leave safely.
**Mission:** find the early signs of a fire, sound a clear alarm and help reduce the danger.

| Target | Requirement |
|---|---|
| Levels | 3 danger levels (e.g. normal, warning, alarm) on the dashboard AND on the board |
| Response | Alarm starts within 20 s after the test condition is applied |
| Reliability | No alarm in normal conditions during the whole demo |
| Reset | Only an allowed person can reset the alarm |

Advanced (optional, inside the 100 points): risk level and how fast it rises; detect a faulty sensor; two rooms on one dashboard. Basics done very well beat unfinished advanced work.

Real-world note from the spec: a real building has many rooms and strict safety rules; a real system is certified, tested often and keeps working when a part fails.

**Safety:** no real fire, flame or smoke. Low voltage only. Water away from wires. If a part breaks, gets hot or smells burnt, stop and call an organizer. Organizers show the safe ways to test.

### Schedule (official)

| Time | Step | What |
|---|---|---|
| 12:00 | Pick Your Property | Draw Property Card and Constraint Card |
| 12:10 | Build Your Strategy | 45 min plan and parts list; submit with **link 1** |
| 12:55 | Race to the Bank | Buy parts; limited stock; plan must be approved first |
| 13:15 | Lay the Foundations | Build hardware |
| 14:00 | Build Your Empire | Code, connect, AI, dashboard: 3 h |
| 17:00 | Final Inspection | Test, pitch prep; juror technical check ~5 min; submit presentation with **link 2** |
| 17:30 | Pass GO, Collect Glory | 8 min: 5 pitch + live demo, 3 questions |

### Scoring (100)

Pitch and demo 30 · Build and prototype 24 · Plan and design 13 · AI 12 · Security 10 · Operator interface 6 · Technical understanding 5.

Evaluation moments: plan check, technical check (show security and AI working, explain your own wiring and code), final presentation.

### Cards

- Constraint Card: adds one rule; the plan must follow it.
- Re-draw once for 50 credits (budget becomes 440), new card final. Teams can swap free if both agree. All before plan check.
- Chance / Community Chest: e.g. teammate out 10 min, pay 20 credits, +30 credits, free part, one mentor question.
- **Jury event:** unannounced failure test: lost sensor, fake command or lost connection. Must handle all three.

### Parts list approval

Sheet complete; total ≤ 490 (440 after re-draw); every part in the price list; at least 2 sensors of different kinds, 1 actuator, 1 board; plan says how the system is protected. First to ask, first served. Faulty-on-arrival parts are replaced. Unspent credits are worth nothing.

### Six-layer minimums

- **Sensing:** 2+ sensors of different kinds; readings follow the physical input; one strange reading must not alarm; faulty or disconnected sensor detected and handled.
- **Communication:** real time, within the response threshold; protected in transit.
- **Edge / AI:** real model reading a window; a simple "if value above" rule is not enough.
- **Actuation:** 1+ actuator reacting by itself; also manual from the interface; defined safe state on failure or lost communication.
- **Security:** protect users, data and commands; 2+ realistic threats with a protection you built.
- **Operator interface:** live values, clear status, alarm indication, event history, send a command; non-expert reads it in seconds.
- **Prototype vs real:** what changes, what is preserved, why representative, unit cost estimate.

### AI standard

1. Window of readings, running live; show window, answer, model file.
2. Test on 20+ labeled windows, normal and abnormal; show test data.
3. Confusion matrix + accuracy, precision, recall.
4. Choose key metric and why; analyse false alarm cost, missed event cost, model limits.
5. Explain inputs, outputs, when it is wrong, why suited. Every member can explain it.

Simple model well explained beats strong model unexplained. Board or laptop scored the same. Comparing with a threshold rule is good but not separately scored.

### Hardware Bank (credits / stock)

Budget: **490 credits**, same for 2 or 3 members. These are event credits.
Free pack: 1 breadboard, 10 M-M + 10 M-F + 10 F-F jumpers, resistors, USB power.

| Part | Price | Stock | | Part | Price | Stock |
|---|---|---|---|---|---|---|
| ESP32 dev board | 150 | 14 | | RFID kit | 90 | 3 |
| Arduino Uno (no Wi-Fi) | 100 | 5 | | Matrix keypad | 50 | 2 |
| Extra wires M-M / M-F / F-F (pack 10) | 5 | 10 each | | Push buttons DS-316MM | 10 | 6 |
| DHT22 | 70 | 8 | | Switch | 10 | 10 |
| Gas sensor (needs warm-up) | 90 | 4 | | Buzzer | 20 | 3 |
| LDR | 30 | 8 | | LED red / green / yellow | 10 | 10 each |
| Water level | 60 | 4 | | 1-channel relay | 80 | 8 |
| Ultrasonic | 60 | 6 | | Servo | 70 | 6 |
| Motion | 80 | 5 | | Mini water pump 12 V | 100 | 3 |
| IR obstacle | 40 | 8 | | Small fan | 80 | 3 |
| MPU6050 | 150 | 2 | | OLED | 120 | 4 |
| LM2596 step-down | 40 | 9 | | | | |

## Decisions

Brief A = "fire intelligence at the first flicker" (@sami). Brief B = "Team System Brief" (@Mayssam Hergli).

**The team chose Brief A's six layers, word for word, and LDR + push button instead of the keypad.** Brief A is now the design of record. Brief B's MQTT contract, keypad, 10 s window and Claude prompts no longer match the sheet; adapt them before use.

Kept from Brief B because they do not conflict: wiring pins, gas sensor voltage divider, fault rules, demo structure, Q&A list.

| Topic | Current decision |
|---|---|
| Sensors | Gas, DHT22, LDR |
| Window | 5 s (10 readings), every 0.5 s |
| AI | Logistic regression on the ESP32, 6 features, outputs normal / gas leak / fire |
| Link | ESP32 → Wi-Fi → laptop gateway, HTTPS (TLS, pinned certificate) |
| Command protection | HMAC-SHA256 + counter |
| Reset | Dashboard login + signed command only. Board button silences for 60 s, never resets |
| Valve | Closes at gas leak or fire; closed on failure or lost link |
| Camera score | Optional (laptop OpenCV) |
| Buzzer fallback | Dashboard siren + red LED strobe (changed from Brief A's LEDs only: the mission needs a clear alarm) |

**Known consequences to manage**

- The fire class must be trained on the organizers' safe test condition. If that test has no flicker or heat, ALARM must still trigger on it.
- DHT22 reads every 2 s, so a 5 s window holds only 2–3 heat points: heat slope is coarse.
- No keypad: reset needs the dashboard link. If the link is down the alarm stays latched until it returns (the safe side).
- Counter must survive an ESP32 reboot (store it in NVS) or commands will be mis-rejected or replayable.

## Final design (EMBER)

One-liner: EMBER is a room detector that spots the early signs of a fire, tells a real danger apart from false alarms, sounds a clear alarm and closes the gas before things get worse.

**BOM, 475 of 490 credits:** ESP32 150 · gas 90 · DHT22 70 · LDR 30 · servo 70 · buzzer 20 · 3 LEDs 30 · push button 10 · extra wires M-F 5. Ask first for gas and buzzer.

**Wiring** (analog on ADC1 only, ADC2 dies with Wi-Fi; shared ground)

| Part | GPIO | Notes |
|---|---|---|
| Gas AO | 34 | Module on 5 V (VIN): 10k / 20k divider so AO stays under 3.3 V. Power early |
| LDR | 35 | LDR to 3.3 V, 10k to GND |
| DHT22 data | 4 | 3.3 V, 10k pull-up if bare |
| Servo signal | 13 | Power from 5 V (VIN) |
| Buzzer | 25 | |
| LED G / Y / R | 26 / 27 / 14 | 220 Ω each |
| Push button | 33 | INPUT_PULLUP, to GND. Silence 60 s only |

Avoid GPIO 0, 2, 5, 12, 15 (strapping pins).

**Logic:** gas alone means a leak (WARNING). Gas + fast heat + flicker means fire (ALARM). A flashlight is bright but steady; a warm hand is heat with no flame. ALARM latches until an authorized reset.

**Faults (from Brief B):** DHT22 no value 3 times → fault. Gas at 0 / 4095 or flat for 30 s → fault. LDR flat → fault. Faulted features go neutral, level at least WARNING with the cause shown.

**AI standard:** 20+ labeled windows, confusion matrix, accuracy, precision, recall. Key metric: fire recall (a missed fire can cost lives; a false alarm costs an evacuation and trust). Compare with a simple threshold rule. Every member must be able to explain the model.

**Targets:** T1 model output → LEDs + banner. T2 0.5 s loop on a 5 s window, target under 10 s (must be measured). T3 trained on traps + 5 s window. T4 dashboard login + signed reset.

**Real product (not in the prototype):**

- Certified multi-sensor detector per room, IR flame sensor, gas solenoid valve, building gateway + cloud dashboard, mains + battery, EN 54 family certification.
- **Voice alerts** that say where the danger is.
- **Interconnection:** all detectors in the building alarm together.
- **Building actions:** gas closed, ventilation off, room power cut.
- **Automatic call to 198** with a spoken brief (room, floor, intensity, gas hazard).
- The prototype must never contact a real emergency number.
- Unit cost 160–260 DT per room is from Brief A, unverified, and does not include the voice and calling features.

**Team checkpoints:** 15:40 live data on the dashboard over TLS · 16:00 data recorded · 16:15 model flashed, levels, reset, signed commands, faults · 16:30 failure drills (unplug a sensor, kill Wi-Fi, unsigned command) · 16:50 rehearse 3 times · 17:00 code freeze · before 17:30 slides via link 2.

**Pitch (5 min):** 0:00 problem · 0:30 EMBER + architecture · 1:00 live demo · 3:30 AI · 4:00 security (4 threats → 4 protections) · 4:30 real deployment (voice alerts, interconnection, 198 call) + unit cost.

**Q&A to prepare:** why logistic regression; Wi-Fi down during a fire; sensor unplugged; stopping remote silencing; why recall; servo on power loss; how reset works if the link is down.
