"""Send timed commands to the board over Serial while printing its output.
Usage: python tools/serial_script.py COM9 DURATION_S [--reset] [--grep P1,P2] "T:cmd" "T:cmd" ...
Example: python tools/serial_script.py COM9 90 --reset --grep ST,#  "25:sim_heat" "60:sim_calm"
Needs SERIAL_CMDS 1 in the firmware. Only one program can hold the port."""
import sys, time, serial

a = sys.argv[1:]
port, dur = a[0], float(a[1])
reset = "--reset" in a
grep = None
if "--grep" in a:
    grep = tuple(a[a.index("--grep") + 1].split(","))
cmds = sorted((float(x.split(":", 1)[0]), x.split(":", 1)[1]) for x in a[2:] if ":" in x and not x.startswith("--"))

s = serial.Serial()
s.port, s.baudrate, s.timeout = port, 115200, 0.1
s.dtr = False; s.rts = False
s.open()
if reset:
    s.rts = True; time.sleep(0.1); s.rts = False
t0 = time.time(); buf = b""
while time.time() - t0 < dur:
    el = time.time() - t0
    while cmds and el >= cmds[0][0]:
        c = cmds.pop(0)[1]
        s.write((c + "\n").encode())
        print(f">>> [{el:5.1f}s] {c}", flush=True)
    buf += s.read(4096)
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        t = line.decode("utf-8", "replace").rstrip("\r")
        if grep is None or t.startswith(grep):
            print(f"[{el:5.1f}] {t}", flush=True)
s.close()
