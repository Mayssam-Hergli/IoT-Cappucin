#!/usr/bin/env python3
"""EMBER camera layer: flame score 0..1 from colour + flicker, sent to Israa's server.

    python cv_flame.py                       # webcam 0, send to server
    python cv_flame.py --tune                # + trackbars for the colour mask
    python cv_flame.py --no-send             # local only
    python cv_flame.py --video clip.mp4      # test on a file (loops)
    python cv_flame.py --model cv_model.joblib   # use the Level 2 model's probability
    python cv_flame.py --yolo                    # Level 3: YOLOv8n fire/smoke detector (fire_yolov8n.onnx)

Keys: q quit, s print current mask parameters.
"""
import argparse
import base64
import hashlib
import hmac
import json
import os
import sys
import threading
import time
from collections import deque

import cv2
import numpy as np

# ---------------------------------------------------------------- config
EMBER_KEY = os.environ.get("EMBER_KEY", "PASTE_32_HEX_KEY_HERE")   # from Israa; never on slides
SERVER_IP = os.environ.get("SERVER_IP", "192.168.43.10")           # Israa's IP on the hotspot
USE_TLS = os.environ.get("USE_TLS", "true").lower() != "false"
SRC_NAME = "cam1"
SEND_EVERY = 0.3          # s between POSTs to /api/cv

FRAME_W, FRAME_H = 320, 240
MIN_BLOB_FRAC = 0.002     # ignore blobs < 0.2 % of the frame
WINDOW = 10               # frames per feature window (~1 s)
EMA_ALPHA = 0.3
FLAME_THRESH = 0.5

# Level 3 detector: YOLOv8n fine-tuned on D-Fire (huggingface.co/rabahdev/fire-smoke-yolov8n, AGPL-3.0),
# exported to ONNX and run through cv2.dnn, so no torch is needed at runtime.
YOLO_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fire_yolov8n.onnx")
YOLO_SIZE = 640
YOLO_CONF = 0.25          # box confidence floor; the EMA + 3-of-5 rule does the rest
YOLO_FIRE_CLASS = 1       # 0 = smoke, 1 = fire

FEATURES = ["area_mean", "area_max", "flicker_mean", "area_var", "bright_mean"]

BASE = f"https://{SERVER_IP}:5443" if USE_TLS else f"http://{SERVER_IP}:5000"


# ---------------------------------------------------------------- scoring
def hand_score(area, flicker, area_var):
    """Level 1 hand-tuned score (before EMA)."""
    a = np.clip(area / 0.02, 0, 1)
    m = np.clip(flicker / 0.08 + area_var / 0.3, 0, 1)
    return float(a * m)


def window_features(hist):
    """hist: sequence of (area_frac, flicker, bright) per frame -> FEATURES vector."""
    h = np.asarray(hist, dtype=np.float64)
    area = h[:, 0]
    mean_a = area.mean()
    area_var = area.std() / mean_a if mean_a > 1e-6 else 0.0
    return [mean_a, area.max(), h[:, 1].mean(), area_var, h[:, 2].mean()]


class YoloFire:
    """YOLOv8n fire detector via cv2.dnn. detect() -> (best fire confidence, boxes in out_size pixels)."""

    def __init__(self, path=YOLO_PATH, size=YOLO_SIZE, conf=YOLO_CONF):
        if not os.path.exists(path):
            sys.exit(f"YOLO model not found: {path}")
        self.net = cv2.dnn.readNetFromONNX(path)
        self.size, self.conf = size, conf

    def detect(self, frame, out_size=(FRAME_W, FRAME_H)):
        h, w = frame.shape[:2]
        scale = self.size / max(h, w)
        nw, nh = round(w * scale), round(h * scale)
        padx, pady = (self.size - nw) // 2, (self.size - nh) // 2
        canvas = np.full((self.size, self.size, 3), 114, np.uint8)       # letterbox
        canvas[pady:pady + nh, padx:padx + nw] = cv2.resize(frame, (nw, nh), interpolation=cv2.INTER_LINEAR)
        self.net.setInput(cv2.dnn.blobFromImage(canvas, 1 / 255.0, (self.size, self.size), swapRB=True))
        out = self.net.forward()[0].T                                     # (8400, 4 + classes)
        conf = out[:, 4 + YOLO_FIRE_CLASS]
        keep = conf >= self.conf
        if not keep.any():
            return 0.0, []
        d, conf = out[keep], conf[keep]
        # cx, cy, w, h in letterboxed pixels -> x, y, w, h in out_size pixels
        k = out_size[0] / w
        x = (d[:, 0] - d[:, 2] / 2 - padx) / scale * k
        y = (d[:, 1] - d[:, 3] / 2 - pady) / scale * k
        bw, bh = d[:, 2] / scale * k, d[:, 3] / scale * k
        rects = np.stack([x, y, bw, bh], axis=1)
        idx = cv2.dnn.NMSBoxes(rects.tolist(), conf.tolist(), self.conf, 0.45)
        idx = np.asarray(idx).reshape(-1)
        boxes = [[int(max(v, 0)) for v in rects[i]] for i in idx]
        return float(conf[idx].max()), boxes


class FlameDetector:
    # mask parameters, tunable live with trackbars (edit here to change the defaults everywhere)
    DEFAULTS = dict(h_hi=35, s_lo=80, v_lo=180, r_min=180)

    def __init__(self, model=None, yolo=None):
        self.p = dict(self.DEFAULTS)
        self.model = model
        self.yolo = yolo
        self.k_open = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (3, 3))
        self.k_close = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (7, 7))
        self.reset()

    def reset(self):
        self.prev_gray = None
        self.prev_clean = np.zeros((FRAME_H, FRAME_W), np.uint8)
        self.hist = deque(maxlen=WINDOW)
        self.recent = deque(maxlen=5)
        self.ema = 0.0

    def mask(self, frame, hsv):
        p = self.p
        m1 = cv2.inRange(hsv, (0, p["s_lo"], p["v_lo"]), (p["h_hi"], 255, 255))
        m2 = cv2.inRange(hsv, (170, p["s_lo"], p["v_lo"]), (180, 255, 255))
        b, g, r = cv2.split(frame)
        rgb_rule = ((r > p["r_min"]) & (r >= g) & (g > b)).astype(np.uint8) * 255
        m = cv2.bitwise_and(cv2.bitwise_or(m1, m2), rgb_rule)
        m = cv2.morphologyEx(m, cv2.MORPH_OPEN, self.k_open)
        m = cv2.morphologyEx(m, cv2.MORPH_CLOSE, self.k_close)
        cnts, _ = cv2.findContours(m, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
        keep = [c for c in cnts if cv2.contourArea(c) >= MIN_BLOB_FRAC * FRAME_W * FRAME_H]
        clean = np.zeros_like(m)
        cv2.drawContours(clean, keep, -1, 255, -1)
        boxes = [list(map(int, cv2.boundingRect(c))) for c in keep]
        return clean, boxes

    def process(self, frame, full=None):
        """frame: BGR 320x240 (`full`: the same frame before downscaling, used by YOLO if given).
        Returns dict with score, flame, boxes, mask and features."""
        hsv = cv2.cvtColor(frame, cv2.COLOR_BGR2HSV)
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        clean, boxes = self.mask(frame, hsv)

        on = clean > 0
        area = float(np.count_nonzero(on)) / (FRAME_W * FRAME_H)
        bright = float(hsv[..., 2][on].mean()) / 255.0 if on.any() else 0.0
        flicker = 0.0
        if self.prev_gray is not None and on.any():
            # Interior shared by this and the previous mask: the edge of a moving solid object
            # (orange mug, hand) is excluded, texture changes inside a flame are kept.
            region = cv2.erode(cv2.bitwise_and(clean, self.prev_clean), self.k_open) > 0
            if region.any():
                flicker = float(cv2.absdiff(gray, self.prev_gray)[region].mean()) / 255.0
        self.prev_gray, self.prev_clean = gray, clean

        self.hist.append((area, flicker, bright))
        h = np.asarray(self.hist)
        a_mean = h[:, 0].mean()
        area_var = float(h[:, 0].std() / a_mean) if a_mean > 1e-6 else 0.0

        if self.yolo is not None:
            raw, boxes = self.yolo.detect(full if full is not None else frame)
        elif self.model is not None:
            raw = 0.0
            if len(self.hist) == WINDOW:
                raw = float(self.model.predict_proba([window_features(self.hist)])[0, 1])
        else:
            raw = hand_score(area, flicker, area_var)
        self.ema = EMA_ALPHA * raw + (1 - EMA_ALPHA) * self.ema
        self.recent.append(self.ema >= FLAME_THRESH)
        flame = sum(self.recent) >= 3

        return dict(score=self.ema, raw=raw, flame=flame, boxes=boxes, mask=clean,
                    area=area, flicker=flicker, area_var=area_var, bright=bright)


# ---------------------------------------------------------------- capture
def open_source(src, lock_exposure=False):
    cap = cv2.VideoCapture(int(src) if str(src).isdigit() else src)
    if not cap.isOpened():
        sys.exit(f"cannot open video source {src!r}")
    if str(src).isdigit():
        cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
        if lock_exposure:
            # V4L2: 1 = manual. DirectShow (Windows): 0.25 = manual. Driver-dependent, may be ignored.
            ok = cap.set(cv2.CAP_PROP_AUTO_EXPOSURE, 0.25 if sys.platform.startswith("win") else 1)
            print("exposure lock:", "accepted" if ok else "refused by driver")
    return cap


def read_frame(cap, is_file=False, with_full=False):
    """Next frame resized to 320x240; with_full=True returns (small, original) instead."""
    ok, frame = cap.read()
    if not ok and is_file:                       # loop video files
        cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
        ok, frame = cap.read()
    if not ok:
        return None
    small = cv2.resize(frame, (FRAME_W, FRAME_H), interpolation=cv2.INTER_AREA)
    return (small, frame) if with_full else small


def annotate(frame, res, fps, link):
    vis = frame.copy()
    colour = (0, 0, 255) if res["flame"] else (0, 165, 255)
    for x, y, w, h in res["boxes"]:
        cv2.rectangle(vis, (x, y), (x + w, y + h), colour, 2)
    label = f"score {res['score']:.2f} {'FLAME' if res['flame'] else ''}"
    cv2.putText(vis, label, (6, 18), cv2.FONT_HERSHEY_SIMPLEX, 0.55, colour, 2)
    cv2.putText(vis, f"{fps:.0f} fps  link:{link}", (6, FRAME_H - 8),
                cv2.FONT_HERSHEY_SIMPLEX, 0.4, (255, 255, 255), 1)
    return vis


# ---------------------------------------------------------------- sender
class Sender(threading.Thread):
    """POSTs the latest score + annotated frame to /api/cv every SEND_EVERY seconds.
    A dead server never blocks the vision loop; failures are retried quietly."""

    def __init__(self):
        super().__init__(daemon=True)
        import requests, urllib3
        urllib3.disable_warnings()
        self.session = requests.Session()
        self.lock = threading.Lock()
        self.latest = None
        self.fresh = False
        self.seq = 0
        self.link = "starting"
        self.stop = threading.Event()

    def update(self, res, fps, vis):
        with self.lock:
            self.latest = (round(res["score"], 3), bool(res["flame"]), res["boxes"], round(fps, 1), vis.copy())
            self.fresh = True

    def run(self):
        import requests
        while not self.stop.wait(SEND_EVERY):
            with self.lock:
                if not self.fresh:
                    continue
                score, flame, boxes, fps, vis = self.latest
                self.fresh = False
            ok, jpg = cv2.imencode(".jpg", vis, [cv2.IMWRITE_JPEG_QUALITY, 60])
            self.seq += 1
            payload = {"src": SRC_NAME, "seq": self.seq, "score": score, "flame": flame,
                       "boxes": boxes, "fps": fps, "jpg": base64.b64encode(jpg.tobytes()).decode()}
            p = json.dumps(payload, separators=(",", ":"))
            sig = hmac.new(EMBER_KEY.encode(), p.encode(), hashlib.sha256).hexdigest()
            try:
                r = self.session.post(f"{BASE}/api/cv", json={"p": p, "sig": sig}, timeout=1, verify=False)
                new = "ok" if r.status_code == 200 else f"http{r.status_code}"
            except requests.RequestException:
                new = "offline"
            if new != self.link:
                print(f"[send] {new}")
                self.link = new


# ---------------------------------------------------------------- main
def add_trackbars(det):
    cv2.namedWindow("tune", cv2.WINDOW_NORMAL)
    for name, hi in (("h_hi", 60), ("s_lo", 255), ("v_lo", 255), ("r_min", 255)):
        cv2.createTrackbar(name, "tune", det.p[name], hi,
                           lambda v, n=name: det.p.__setitem__(n, v))
    cv2.resizeWindow("tune", 400, 160)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default="0", help="camera index or video file")
    ap.add_argument("--tune", action="store_true", help="live trackbars for the colour mask")
    ap.add_argument("--no-send", action="store_true")
    ap.add_argument("--headless", action="store_true", help="no window")
    ap.add_argument("--lock-exposure", action="store_true")
    ap.add_argument("--model", help="joblib model from cv_train.py (uses its probability as the score)")
    ap.add_argument("--yolo", nargs="?", const=YOLO_PATH, metavar="ONNX",
                    help="use the YOLOv8n fire detector as the score (default file: fire_yolov8n.onnx)")
    ap.add_argument("--scale", type=float, default=2.0, help="window zoom")
    args = ap.parse_args()

    model = None
    if args.model:
        import joblib
        model = joblib.load(args.model)
        print("using model", args.model)

    yolo = None
    if args.yolo:
        yolo = YoloFire(args.yolo)
        print("using YOLO", args.yolo)
    det = FlameDetector(model, yolo)
    is_file = not str(args.src).isdigit()
    cap = open_source(args.src, args.lock_exposure)
    file_dt = 1.0 / (cap.get(cv2.CAP_PROP_FPS) or 25) if is_file else 0

    sender = None
    if args.no_send:
        pass
    elif EMBER_KEY.startswith("PASTE"):
        print("EMBER_KEY not set -> running locally only (set EMBER_KEY env var or edit the file)")
    else:
        sender = Sender()
        sender.start()
        print("sending to", BASE)

    show = not args.headless
    if show and args.tune:
        add_trackbars(det)

    fps, t_prev = 0.0, time.time()
    try:
        while True:
            got = read_frame(cap, is_file, with_full=True)
            if got is None:
                print("camera read failed")
                break
            frame, full = got
            res = det.process(frame, full)
            now = time.time()
            fps = 0.9 * fps + 0.1 / max(now - t_prev, 1e-3) if fps else 1 / max(now - t_prev, 1e-3)
            t_prev = now

            vis = annotate(frame, res, fps, sender.link if sender else "off")
            if sender:
                sender.update(res, fps, vis)
            if show:
                panel = np.hstack([vis, cv2.cvtColor(res["mask"], cv2.COLOR_GRAY2BGR)])
                dbg = f"area {res['area']:.3f} flick {res['flicker']:.3f} var {res['area_var']:.2f} bright {res['bright']:.2f}"
                cv2.putText(panel, dbg, (6, 36), cv2.FONT_HERSHEY_SIMPLEX, 0.4, (200, 255, 200), 1)
                if args.scale != 1:
                    panel = cv2.resize(panel, None, fx=args.scale, fy=args.scale)
                cv2.imshow("EMBER camera", panel)
                k = cv2.waitKey(1) & 0xFF
                if k == ord("q"):
                    break
                if k == ord("s"):
                    print("params:", det.p)
            if file_dt:
                time.sleep(file_dt)
    except KeyboardInterrupt:
        pass
    finally:
        if sender:
            sender.stop.set()
        cap.release()
        cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
