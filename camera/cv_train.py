#!/usr/bin/env python3
"""Train the camera model from cv_data.csv, split BY CLIP, and compare with the hand-tuned score.

    python cv_train.py [--seed 0] [--final]

Writes cv_model.joblib and cv_confusion.png. --final refits on all clips after reporting
(metrics stay those of the held-out clips).
"""
import argparse

import joblib
import matplotlib
import numpy as np
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import accuracy_score, confusion_matrix, precision_score, recall_score
from sklearn.pipeline import make_pipeline
from sklearn.preprocessing import StandardScaler

from cv_flame import FEATURES, FLAME_THRESH, hand_score

matplotlib.use("Agg")
import matplotlib.pyplot as plt

MIN_TEST_WINDOWS = 20


class Data:
    """Minimal table: clip_id, label, X (windows x FEATURES)."""

    def __init__(self, clip_id, label, X):
        self.clip_id, self.label, self.X = clip_id, label, X

    def __len__(self):
        return len(self.label)

    def sub(self, mask):
        return Data(self.clip_id[mask], self.label[mask], self.X[mask])


def load(path):
    a = np.loadtxt(path, delimiter=",", skiprows=1, ndmin=2)
    return Data(a[:, 0].astype(int), a[:, 1].astype(int), a[:, 2:])


def split_by_clip(df, seed, test_frac=0.3):
    """Hold out whole clips, stratified by label, so near-identical frames never straddle the split."""
    rng = np.random.default_rng(seed)
    test_clips = []
    for lbl in (0, 1):
        clips = np.unique(df.clip_id[df.label == lbl])
        if len(clips) < 2:
            raise SystemExit(f"need >= 2 clips with label {lbl} (have {len(clips)}); record more")
        rng.shuffle(clips)
        test_clips += list(clips[:max(1, round(len(clips) * test_frac))])
    is_test = np.isin(df.clip_id, test_clips)
    return df.sub(~is_test), df.sub(is_test)


def metrics(y, pred):
    return dict(acc=accuracy_score(y, pred),
                prec=precision_score(y, pred, zero_division=0),
                rec=recall_score(y, pred, zero_division=0),
                cm=confusion_matrix(y, pred, labels=[0, 1]))


def plot_cm(cm, title, ax):
    ax.imshow(cm, cmap="Oranges")
    ax.set_xticks([0, 1], ["no flame", "flame"])
    ax.set_yticks([0, 1], ["no flame", "flame"])
    ax.set_xlabel("predicted")
    ax.set_ylabel("actual")
    ax.set_title(title)
    for i in range(2):
        for j in range(2):
            ax.text(j, i, cm[i, j], ha="center", va="center", fontsize=16)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default="cv_data.csv")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--final", action="store_true")
    args = ap.parse_args()

    df = load(args.data)
    print(f"{len(df)} windows, {len(set(df.clip_id))} clips "
          f"(flame {int((df.label == 1).sum())}, no-flame {int((df.label == 0).sum())})")
    train, test = split_by_clip(df, args.seed)
    print(f"train: {len(train)} windows / {len(set(train.clip_id))} clips   "
          f"test: {len(test)} windows / {len(set(test.clip_id))} clips")
    if len(test) < MIN_TEST_WINDOWS:
        print(f"WARNING: only {len(test)} test windows (< {MIN_TEST_WINDOWS}); record more or longer clips")

    model = make_pipeline(StandardScaler(), LogisticRegression(class_weight="balanced", max_iter=1000))
    model.fit(train.X, train.label)

    y = test.label
    m_model = metrics(y, (model.predict_proba(test.X)[:, 1] >= FLAME_THRESH).astype(int))
    ix = {n: i for i, n in enumerate(FEATURES)}
    hand = [hand_score(x[ix["area_mean"]], x[ix["flicker_mean"]], x[ix["area_var"]]) >= FLAME_THRESH for x in test.X]
    m_hand = metrics(y, np.array(hand, dtype=int))

    print(f"\n{'':14}{'accuracy':>10}{'precision':>11}{'recall':>9}")
    for name, m in (("model", m_model), ("hand-tuned", m_hand)):
        print(f"{name:14}{m['acc']:>10.3f}{m['prec']:>11.3f}{m['rec']:>9.3f}")
    print("\nmodel confusion matrix [rows actual 0/1, cols predicted 0/1]:\n", m_model["cm"])

    coefs = model[-1].coef_[0]
    print("\nstandardised coefficients:", {n: round(float(c), 2) for n, c in zip(FEATURES, coefs)})

    fig, axs = plt.subplots(1, 2, figsize=(9, 4))
    plot_cm(m_model["cm"], f"Camera model (precision {m_model['prec']:.2f})", axs[0])
    plot_cm(m_hand["cm"], f"Hand-tuned (precision {m_hand['prec']:.2f})", axs[1])
    fig.suptitle(f"Held-out clips: {len(test)} windows from {len(set(test.clip_id))} clips")
    fig.tight_layout()
    fig.savefig("cv_confusion.png", dpi=150)

    better = (m_model["prec"], m_model["rec"]) > (m_hand["prec"], m_hand["rec"])
    if args.final:
        model.fit(df.X, df.label)
    joblib.dump(model, "cv_model.joblib")
    print("\nsaved cv_model.joblib, cv_confusion.png")
    print("model beats hand-tuned on precision (then recall)" if better else
          "hand-tuned is at least as good -> keep it (don't pass --model)")
    if better:
        print("use it: python cv_flame.py --model cv_model.joblib")


if __name__ == "__main__":
    main()
