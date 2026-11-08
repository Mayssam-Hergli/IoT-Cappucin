"""Read a serial port for N seconds and print the lines.
Usage: python tools/serial_read.py [PORT] [SECONDS] [--reset] [--grep PREFIX]
Opening does NOT reset the ESP32 unless --reset is given (pulses EN via RTS).
Only one program can hold the port: close the Arduino Serial Monitor first."""
import sys, time, serial

args = [a for a in sys.argv[1:] if not a.startswith("--")]
port = args[0] if len(args) > 0 else "COM9"
secs = float(args[1]) if len(args) > 1 else 10
grep = None
if "--grep" in sys.argv:
    grep = sys.argv[sys.argv.index("--grep") + 1]
    if grep in args: args.remove(grep)

def open_port():
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dtr = False; s.rts = False          # keep EN and IO0 released
    s.open()
    return s

s = open_port()
if "--reset" in sys.argv:
    s.rts = True; time.sleep(0.1); s.rts = False
end = time.time() + secs
buf = b""
while time.time() < end:
    try:
        buf += s.read(4096)
    except serial.SerialException:
        print("# [serial_read] PORT LOST", time.strftime("%H:%M:%S"), flush=True)
        try: s.close()
        except Exception: pass
        while time.time() < end:
            time.sleep(0.5)
            try: s = open_port(); print("# [serial_read] port back", flush=True); break
            except serial.SerialException: pass
        continue
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        t = line.decode("utf-8", "replace").rstrip("\r")
        if grep is None or t.startswith(grep):
            print(t, flush=True)
s.close()
