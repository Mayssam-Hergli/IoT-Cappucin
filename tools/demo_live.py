"""One-take live demo driver: ALARM -> WARNING -> NORMAL. Survives USB drops (servo)."""
import sys, time, serial
PORT = sys.argv[1] if len(sys.argv) > 1 else "COM9"
PLAN = [(0, "valve_open"), (3, "!reboot"), (26, "sim_flame"), (40, "sim_heat"), (46, "reset"), (58, "sim_calm"), (76, "valve_open")]
END = 82


def op():
    s = serial.Serial(); s.port, s.baudrate, s.timeout = PORT, 115200, 0.1
    s.dtr = False; s.rts = False; s.open(); return s


s = op(); t0 = time.time(); buf = b""; last = None
print("t=0  START RECORDING NOW", flush=True)
while time.time() - t0 < END:
    el = time.time() - t0
    if PLAN and el >= PLAN[0][0]:
        c = PLAN[0][1]
        try:
            if c == "!reboot":              # hardware reset via EN (RTS): sim temperature back to 24 °C
                s.rts = True; time.sleep(0.1); s.rts = False
            else:
                s.write((c + "\n").encode())
            PLAN.pop(0); print(f"[{el:4.0f}s] >>> {c}", flush=True)
        except Exception:
            pass
    try:
        buf += s.read(4096)
    except Exception:
        print(f"[{el:4.0f}s] (usb dropped, reconnecting)", flush=True)
        try: s.close()
        except Exception: pass
        while time.time() - t0 < END:
            time.sleep(0.3)
            try: s = op(); print(f"[{time.time()-t0:4.0f}s] (usb back)", flush=True); break
            except Exception: pass
        continue
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1); t = line.decode("utf-8", "replace").strip()
        if t.startswith("# EVT") or t.startswith("rst:") or "Brownout" in t:
            print(f"[{el:4.0f}s] {t}", flush=True)
        elif t.startswith("ST,"):
            st = ",".join(t.split(",")[1:3])
            if st != last: last = st; print(f"[{el:4.0f}s] STATE {st}", flush=True)
print("END", flush=True)
