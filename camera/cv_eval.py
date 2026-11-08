#!/usr/bin/env python3
"""Score the YOLO camera model on the windows recorded with `cv_record.py --yolo`.

    python cv_eval.py [--data data/cv_yolo_data.csv] [--min-hits 3]

YOLO is pretrained (not trained by us), so every recorded window is test data and there is no train split.
A window = WINDOW frames. YOLO says FLAME for a window when it saw fire (confidence >= 0.5) in at least
--min-hits of its frames; the colour+flicker rule says FLAME when its hand-tuned score >= FLAME_THRESH.
Writes results/cv_yolo_confusion.png.
"""
import argparse
import os

import matplotlib
import numpy as np

from cv_flame import FEATURES, FLAME_THRESH, WINDOW, hand_score

matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data", "cv_yolo_data.csv")
PNG = os.path.join(HERE, "results", "cv_yolo_confusion.png")
MIN_TEST_WINDOWS = 20


def load(path):
    with open(path) as f:
        cols = f.readline().strip().split(",")
    a = np.loadtxt(path, delimiter=",", skiprows=1, ndmin=2)
    return {c: a[:, i] for i, c in enumerate(cols)}


def metrics(y, pred):
    tp = int(((y == 1) & (pred == 1)).sum())
    tn = int(((y == 0) & (pred == 0)).sum())
    fp = int(((y == 0) & (pred == 1)).sum())
    fn = int(((y == 1) & (pred == 0)).sum())
    div = lambda a, b: a / b if b else 0.0
    return dict(tp=tp, tn=tn, fp=fp, fn=fn, acc=div(tp + tn, len(y)),
                prec=div(tp, tp + fp), rec=div(tp, tp + fn),
                cm=np.array([[tn, fp], [fn, tp]]))


def plot_cm(m, title, ax):
    ax.imshow(m["cm"], cmap="Oranges")
    ax.set_xticks([0, 1], ["no flame", "flame"])
    ax.set_yticks([0, 1], ["no flame", "flame"])
    ax.set_xlabel("predicted")
    ax.set_ylabel("actual")
    ax.set_title(title)
    for i in range(2):
        for j in range(2):
            ax.text(j, i, m["cm"][i, j], ha="center", va="center", fontsize=16)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=DATA)
    ap.add_argument("--min-hits", type=int, default=3,
                    help=f"frames (out of {WINDOW}) that must show fire for YOLO to call the window FLAME")
    args = ap.parse_args()

    if not os.path.exists(args.data):
        raise SystemExit(f"{args.data} not found: nothing recorded yet.\n"
                         "Record clips first:  python cv_record.py --yolo   (f = flame, n = no flame, space = stop, q = quit)")
    d = load(args.data)
    y = d["label"].astype(int)
    clip = d["clip_id"].astype(int)
    n1, n0 = int((y == 1).sum()), int((y == 0).sum())
    print(f"{len(y)} windows from {len(set(clip))} clips  (flame {n1}, no-flame {n0})")
    if n1 == 0 or n0 == 0:
        raise SystemExit("need both flame and no-flame windows; record the missing kind")
    if len(y) < MIN_TEST_WINDOWS:
        print(f"WARNING: only {len(y)} windows (< {MIN_TEST_WINDOWS}); record more or longer clips")

    yolo = (d["yolo_hits"] >= args.min_hits).astype(int)
    hand = np.array([hand_score(a, f, v) >= FLAME_THRESH
                     for a, f, v in zip(d["area_mean"], d["flicker_mean"], d["area_var"])], dtype=int)
    m_yolo, m_hand = metrics(y, yolo), metrics(y, hand)

    print(f"\n{'':22}{'accuracy':>10}{'precision':>11}{'recall':>9}")
    for name, m in ((f"YOLO (>= {args.min_hits}/{WINDOW} frames)", m_yolo), ("colour + flicker rule", m_hand)):
        print(f"{name:22}{m['acc']:>10.2f}{m['prec']:>11.2f}{m['rec']:>9.2f}")
    print(f"\nYOLO: {m_yolo['tp']} flames found, {m_yolo['fn']} flames missed, "
          f"{m_yolo['fp']} false alarms, {m_yolo['tn']} correct 'no flame'")

    print("\nsame data, other --min-hits values (YOLO only):")
    for k in range(1, WINDOW // 2 + 1):
        m = metrics(y, (d["yolo_hits"] >= k).astype(int))
        print(f"  min-hits {k}: precision {m['prec']:.2f}  recall {m['rec']:.2f}  false alarms {m['fp']}")

    print("\nper clip (YOLO):  clip  actual     windows  called-flame  correct")
    for c in sorted(set(clip)):
        sel = clip == c
        print(f"                 {c:>4}  {'flame' if y[sel][0] else 'no-flame':<9}{int(sel.sum()):>8}{int(yolo[sel].sum()):>14}"
              f"{(yolo[sel] == y[sel]).mean():>9.0%}")

    wrong = np.flatnonzero(yolo != y)
    if len(wrong):
        print("\nwhere YOLO is wrong (clip: windows):")
        for c in sorted(set(clip[wrong])):
            sel = wrong[clip[wrong] == c]
            kind = "flame clip, missed" if y[sel[0]] == 1 else "no-flame clip, false alarm"
            print(f"  clip {c}: {len(sel)} of {int((clip == c).sum())} windows  ({kind})")

    fig, axs = plt.subplots(1, 2, figsize=(9, 4))
    plot_cm(m_yolo, f"YOLO (precision {m_yolo['prec']:.2f}, recall {m_yolo['rec']:.2f})", axs[0])
    plot_cm(m_hand, f"Colour+flicker (precision {m_hand['prec']:.2f}, recall {m_hand['rec']:.2f})", axs[1])
    fig.suptitle(f"{len(y)} labelled windows from {len(set(clip))} clips")
    fig.tight_layout()
    os.makedirs(os.path.dirname(PNG), exist_ok=True)
    fig.savefig(PNG, dpi=150)
    print(f"\nsaved {PNG}")
    print("""
How to read it:
  precision = when the camera says FLAME, how often is it really a flame   (key metric: few false alarms)
  recall    = of all real flame windows, how many the camera caught
  accuracy  = overall share of windows it got right""")


if __name__ == "__main__":
    main()
