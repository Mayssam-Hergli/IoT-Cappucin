"""Record EMBER training data from the board's DATA lines.
Usage:  python record_serial.py COM5 normal 90
Labels: normal | flashlight | handheat | gas | fire   (file prefix = label)
Close the Arduino Serial Monitor first (otherwise the port is busy).
"""
import os, sys, time

import serial

COLS = ["up_ms", "gas_raw", "temp", "hum", "ldr_raw", "gas_delta", "gas_slope",
        "temp_slope", "light_delta", "flicker", "level"]
LABELS = {"normal", "flashlight", "handheat", "gas", "fire"}

def main():
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(1)
    port, label = sys.argv[1], sys.argv[2].lower()
    secs = float(sys.argv[3]) if len(sys.argv) > 3 else 90
    if label not in LABELS:
        print(f"Unknown label '{label}'. Use one of: {', '.join(sorted(LABELS))}"); sys.exit(1)

    os.makedirs("data", exist_ok=True)
    path = os.path.join("data", f"{label}_{time.strftime('%H%M%S')}.csv")
    ser = serial.Serial(port, 115200, timeout=1)
    ser.reset_input_buffer()

    print(f"Recording '{label}' for {secs:.0f} s from {port} -> {path}")
    if label != "normal":
        print("Apply the stimulus NOW and keep it there. Starting in 3 s...")
    for i in (3, 2, 1):
        print(f"  {i}..."); time.sleep(1)
    ser.reset_input_buffer()

    n, t0, last_show = 0, time.time(), 0
    with open(path, "w", encoding="utf-8") as f:
        f.write(",".join(COLS) + "\n")
        while time.time() - t0 < secs:
            line = ser.readline().decode("utf-8", errors="ignore").strip()
            if not line.startswith("DATA,"):
                if line.startswith("#"):
                    print("   board:", line)
                continue
            parts = line[5:].split(",")
            if len(parts) != len(COLS):
                continue
            f.write(",".join(parts) + "\n")
            n += 1
            if time.time() - last_show >= 2:
                last_show = time.time()
                d = dict(zip(COLS, parts))
                print(f"  {time.time()-t0:5.0f}s rows={n:4d} gas_delta={d['gas_delta']:>8} "
                      f"gas_slope={d['gas_slope']:>7} temp_slope={d['temp_slope']:>7} level={d['level']}")
    ser.close()
    print(f"Saved {n} rows to {path}")
    if n < secs:   # expect ~2 rows per second
        print("WARNING: fewer rows than expected. Is the board printing DATA lines at 115200 baud?")

if __name__ == "__main__":
    main()
