#!/usr/bin/env python3
"""Record labelled 1-second windows for the camera model (Level 2).

    python cv_record.py            # appends to data/cv_data.csv (colour/flicker features, for cv_train.py)
    python cv_record.py --yolo     # appends to data/cv_yolo_data.csv (colour features + YOLO), then run cv_eval.py

Keys:  f = start a FLAME clip      n = start a NO-FLAME clip
       space = stop the clip       q = quit
Every start gives a new clip id (the model is split by clip, not by frame).
Record clips of >= 20 s. Vary distance and angle for flame (phone fire video);
make no-flame hard: empty scene, people moving, red/orange objects, bright non-fire video, flashlight.
"""
import argparse
import csv
import os

import cv2
import numpy as np

from cv_flame import FEATURES, WINDOW, FlameDetector, YoloFire, open_source, read_frame, window_features

DATA_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
OUT = os.path.join(DATA_DIR, "cv_data.csv")
YOLO_OUT = os.path.join(DATA_DIR, "cv_yolo_data.csv")
YOLO_COLS = ["yolo_mean", "yolo_max", "yolo_hits"]    # per window: mean / max fire confidence, frames >= YOLO_HIT
YOLO_HIT = 0.5
HEADER = ["clip_id", "label"] + FEATURES


def next_clip_id(path):
    if not os.path.exists(path):
        return 1
    with open(path) as f:
        ids = [int(r["clip_id"]) for r in csv.DictReader(f)]
    return max(ids, default=0) + 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default="0")
    ap.add_argument("--lock-exposure", action="store_true")
    ap.add_argument("--yolo", action="store_true", help="also record the YOLO fire confidence (cv_yolo_data.csv)")
    args = ap.parse_args()

    out = YOLO_OUT if args.yolo else OUT
    os.makedirs(DATA_DIR, exist_ok=True)
    cap = open_source(args.src, args.lock_exposure)
    det = FlameDetector(None, YoloFire() if args.yolo else None)
    new_file = not os.path.exists(out)
    f = open(out, "a", newline="")
    w = csv.writer(f)
    if new_file:
        w.writerow(HEADER + (YOLO_COLS if args.yolo else []))

    clip_id = next_clip_id(out)
    label = None            # None = idle, 1 = flame, 0 = no flame
    buf = []
    counts = {0: 0, 1: 0}
    clip_windows = 0

    def start(lbl):
        nonlocal label, buf, clip_windows, clip_id
        if label is not None:                # already recording: finish that clip first
            clip_id += 1
        label, buf, clip_windows = lbl, [], 0
        det.reset()                          # windows never straddle two clips

    def stop():
        nonlocal label, buf, clip_id
        if label is not None:
            print(f"clip {clip_id} ({'flame' if label else 'no-flame'}): {clip_windows} windows")
            clip_id += 1
        label, buf = None, []

    while True:
        got = read_frame(cap, not str(args.src).isdigit(), with_full=True)
        if got is None:
            break
        frame, full = got
        res = det.process(frame, full)
        if label is not None:
            buf.append((res["area"], res["flicker"], res["bright"], res["raw"]))
            if len(buf) == WINDOW:
                row = [f"{v:.6f}" for v in window_features(buf)]
                if args.yolo:
                    c = np.array([b[3] for b in buf])
                    row += [f"{c.mean():.6f}", f"{c.max():.6f}", int((c >= YOLO_HIT).sum())]
                w.writerow([clip_id, label] + row)
                f.flush()
                counts[label] += 1
                clip_windows += 1
                buf = []

        vis = frame.copy()
        for x, y, bw, bh in res["boxes"]:
            cv2.rectangle(vis, (x, y), (x + bw, y + bh), (0, 165, 255), 2)
        status = "IDLE" if label is None else ("REC FLAME" if label else "REC NO-FLAME")
        colour = (200, 200, 200) if label is None else ((0, 0, 255) if label else (0, 200, 0))
        cv2.putText(vis, f"{status} clip {clip_id} win {clip_windows}", (6, 18),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.55, colour, 2)
        if args.yolo:
            cv2.putText(vis, f"yolo fire {res['raw']:.2f}", (6, 36), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 255), 1)
        cv2.putText(vis, f"total flame {counts[1]}  no-flame {counts[0]}", (6, 234),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.45, (255, 255, 255), 1)
        cv2.imshow("EMBER record", cv2.resize(vis, None, fx=2, fy=2))
        k = cv2.waitKey(1) & 0xFF
        if k == ord("q"):
            break
        if k == ord("f") and label != 1:
            start(1)
        elif k == ord("n") and label != 0:
            start(0)
        elif k == ord(" "):
            stop()

    stop()
    f.close()
    cap.release()
    cv2.destroyAllWindows()
    print(f"saved to {out}: flame {counts[1]} windows, no-flame {counts[0]} windows this session")


if __name__ == "__main__":
    main()
