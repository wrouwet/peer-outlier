#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Plot the cmis_drift dataset: one aging optical module among 32, and pod catching it.

usage: plot.py [data_dir] [output.png]
"""
import csv
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

data_dir = sys.argv[1] if len(sys.argv) > 1 else "data"
out = sys.argv[2] if len(sys.argv) > 2 else "cmis-drift.png"

TARGET = 19
AGING_START = 120
COOLING_DAY = 200

# Palette: emphasis on one series, peers recede to gray.
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK2 = "#52514e"
GRID = "#e4e3df"
PEER = "#b9b8b2"
FOCUS = "#2a78d6"     # module 19
ALERT = "#d03b3b"     # status: critical
CUTOFF = "#52514e"

with open(f"{data_dir}/timeseries.csv") as f:
    rows = list(csv.DictReader(f))
day = np.array([float(r["day"]) for r in rows])
cutoff = float(rows[0]["cutoff"])
n = sum(1 for k in rows[0] if k.startswith("bias_"))
bias = np.array([[float(r[f"bias_{m}"]) for r in rows] for m in range(n)])
z = np.array([[float(r[f"z_{m}"]) for r in rows] for m in range(n)])

with open(f"{data_dir}/alerts.csv") as f:
    alerts = list(csv.DictReader(f))


def alert_spans(module, view):
    spans, start = [], None
    for a in alerts:
        if int(a["module"]) != module or a["view"] != view:
            continue
        if a["state"] == "set":
            start = float(a["day"])
        elif start is not None:
            spans.append((start, float(a["day"])))
            start = None
    if start is not None:
        spans.append((start, 365.0))
    return spans


first = next(a for a in alerts if int(a["module"]) == TARGET and a["view"] == "changing")
alert_day = float(first["day"])
alert_deg = float(first["degradation"]) * 100
others_changing = [a for a in alerts if int(a["module"]) != TARGET and a["view"] == "changing"]

plt.rcParams.update({
    "font.family": "DejaVu Sans", "font.size": 10, "axes.edgecolor": GRID,
    "axes.labelcolor": INK2, "xtick.color": INK2, "ytick.color": INK2,
    "axes.titlesize": 11, "axes.titleweight": "bold", "axes.titlecolor": INK,
    "axes.titlelocation": "left", "figure.facecolor": SURFACE, "axes.facecolor": SURFACE,
})

fig = plt.figure(figsize=(12, 13.5))
gs = fig.add_gridspec(4, 1, height_ratios=[3, 3, 3, 1.6], hspace=0.55, top=0.9)
peers = [m for m in range(n) if m != TARGET]


def style(ax):
    ax.grid(True, color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)


def mark_alert(ax, label=True):
    ax.axvline(alert_day, color=ALERT, linewidth=1.5, linestyle="-")
    if label:
        ax.annotate(f"pod alert: day {alert_day:.1f}", xy=(alert_day, 1), xycoords=("data", "axes fraction"),
                    xytext=(6, -14), textcoords="offset points", color=ALERT, fontsize=9, fontweight="bold")


# A. What a dashboard shows: raw Tx bias of every module.
ax = fig.add_subplot(gs[0])
style(ax)
for m in peers:
    ax.plot(day, bias[m], color=PEER, linewidth=0.6, alpha=0.7)
ax.plot(day, bias[TARGET], color=FOCUS, linewidth=1.6)
ax.axvspan(AGING_START, 365, color=FOCUS, alpha=0.05)
mark_alert(ax)
ax.annotate("cooling failure\n(all modules)", xy=(COOLING_DAY + 1, np.max(bias[:, (day > COOLING_DAY) & (day < COOLING_DAY + 2)])),
            xytext=(COOLING_DAY + 18, np.max(bias) - 1), color=INK2, fontsize=9,
            arrowprops=dict(arrowstyle="-", color=INK2, linewidth=0.8))
ax.text(366, bias[TARGET][-1], f" module {TARGET}", color=FOCUS, va="center", fontsize=9, fontweight="bold")
ax.text(366, np.median(bias[peers, -1]), f" {n - 1} other\n modules", color=INK2, va="center", fontsize=9)
ax.text(AGING_START + 2, ax.get_ylim()[0] + 0.5, f"module {TARGET}'s laser starts aging (day {AGING_START})",
        color=FOCUS, fontsize=9)
ax.set_xlim(0, 365)
ax.set_ylabel("Tx bias (mA)")
ax.set_title("A. The raw data: laser bias current of 32 optical modules over a year (hourly samples, every 6 h shown)")

# B. Zoom on the onset, peer-relative.
ax = fig.add_subplot(gs[1])
style(ax)
win = (day >= 100) & (day <= 150)
med = np.median(bias[:, win], axis=0)
for m in peers:
    ax.plot(day[win], bias[m, win] - med, color=PEER, linewidth=0.6, alpha=0.7)
ax.plot(day[win], bias[TARGET, win] - med, color=FOCUS, linewidth=1.8)
ax.axvline(AGING_START, color=FOCUS, linewidth=1, linestyle=":")
mark_alert(ax)
inc = 14.0 * float(first["degradation"])
ax.text(AGING_START + 0.4, ax.get_ylim()[1] * 0.85, "aging starts", color=FOCUS, fontsize=9)
ax.text(alert_day + 0.6, ax.get_ylim()[0] * 0.85,
        f"caught at {alert_deg:.1f}% of end-of-life degradation:\nbias only +{inc:.2f} mA above its own normal",
        color=INK, fontsize=9, bbox=dict(facecolor=SURFACE, edgecolor="none", pad=2))
ax.set_xlim(100, 150)
ax.set_ylabel("bias minus fleet median (mA)")
ax.set_title("B. Zoom on days 100-150, each module relative to the fleet median: the drift is tiny and buried in each module's own spread")

# C. What pod computes.
ax = fig.add_subplot(gs[2])
style(ax)
zc = np.clip(z, -12, 30)
for m in peers:
    ax.plot(day, zc[m], color=PEER, linewidth=0.6, alpha=0.7)
ax.plot(day, zc[TARGET], color=FOCUS, linewidth=1.4)
ax.axhline(cutoff, color=CUTOFF, linestyle="--", linewidth=1)
ax.axhline(-cutoff, color=CUTOFF, linestyle="--", linewidth=1)
ax.text(2, -cutoff - 2.6, f"cutoff ±{cutoff:.1f}: derived from group size (Bonferroni, 1% per snapshot), not programmed",
        color=INK2, fontsize=9, bbox=dict(facecolor=SURFACE, edgecolor="none", pad=1))
mark_alert(ax, label=False)
ax.set_xlim(0, 365)
ax.set_ylim(-12, 30)
ax.set_ylabel("robust z vs peers")
ax.set_title("C. What pod sees: each module's Tx-bias change (fast minus robust 1-week average), as a robust z among its peers")

# D. Alert timeline.
ax = fig.add_subplot(gs[3])
lanes = [
    (f"module {TARGET} CHANGING", alert_spans(TARGET, "changing"), ALERT),
    (f"module {TARGET} DIFFERENT", alert_spans(TARGET, "different"), FOCUS),
    ("module 7 DIFFERENT\n(healthy, long fibre)", alert_spans(7, "different"), PEER),
    (f"other {n - 2} modules", [], PEER),
]
for i, (label, spans, color) in enumerate(lanes):
    y = len(lanes) - 1 - i
    for s, e in spans:
        ax.barh(y, e - s, left=s, height=0.55, color=color)
    ax.text(-4, y, label, ha="right", va="center", fontsize=9, color=INK)
    if not spans:
        ax.text(2, y, "no alerts all year" if not others_changing else f"{len(others_changing)} alerts",
                va="center", fontsize=9, color=INK2)
ax.set_xlim(0, 365)
ax.set_ylim(-0.7, len(lanes) - 0.3)
ax.set_yticks([])
for s in ("top", "right", "left"):
    ax.spines[s].set_visible(False)
ax.set_xlabel("day of year")
ax.set_title("D. pod alerts (an alert = stood out in 18 of the last 24 hourly cycles)")

fig.text(0.06, 0.965, "pod catching one aging optical module among 32: no training, no thresholds",
         ha="left", fontsize=14, fontweight="bold", color=INK)
fig.text(0.06, 0.93,
         "Simulated CMIS telemetry: daily and seasonal temperature, per-module spread, noise, a 3-hour glitch "
         "(module 3, day 150), a 2-day cooling failure (day 200).\n"
         f"Module {TARGET}'s laser ages from day {AGING_START} (bias +14 mA by day 365). pod reads the raw 256-byte "
         "CMIS records once an hour.",
         ha="left", fontsize=9.5, color=INK2)
fig.savefig(out, dpi=130, bbox_inches="tight", facecolor=SURFACE)
print(f"wrote {out}")
