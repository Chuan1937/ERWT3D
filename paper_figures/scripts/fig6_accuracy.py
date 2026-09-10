"""Fig6: RZFP error-bound verification.

(a) Maximum relative error with error bound threshold
(b) NRMSE

Source: validation/summaries_final/accuracy.csv
"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
import style
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

DATA_DIR = os.path.join(os.path.dirname(__file__), "..", "..", "validation", "summaries_final")

def make_figure(outdir="paper_figures/output"):
    acc = pd.read_csv(os.path.join(DATA_DIR, "accuracy.csv"))
    rzfp = acc[acc["format"] == "RZFP"].copy()

    fig, axes = plt.subplots(1, 2, figsize=(style.WIDTH_DOUBLE, 50/25.4))

    datasets = rzfp["dataset"].values
    violations = rzfp["violation_count"].values.astype(int)

    # ====================================================================
    # (a) Maximum relative error
    # ====================================================================
    ax = axes[0]
    style.panel_label(ax, "(a)")

    max_rel = rzfp["max_relative_error"].values.astype(float)
    x = np.arange(len(datasets))

    bars = ax.bar(x, max_rel, width=0.45, color=style.COLORS["rzfp"],
                  edgecolor="#333333", linewidth=0.5)

    # Error bound line (prominent)
    bound = 1e-3
    ax.axhline(y=bound, color="#C0392B", linestyle="--", linewidth=1.2, zorder=5)
    ax.text(len(datasets) - 0.5, bound * 1.08, f"Contest bound\n$1 \\times 10^{{-3}}$",
            ha="right", va="bottom", fontsize=5.5, fontweight="bold", color="#C0392B")

    # Value labels
    for i, (v, vi) in enumerate(zip(max_rel, violations)):
        ax.text(i, v + bound * 0.06, f"{v:.3e}\n({vi} violations)",
                ha="center", va="bottom", fontsize=5.5)

    ax.set_xticks(x)
    ax.set_xticklabels(datasets, fontsize=6)
    ax.set_ylabel("Maximum relative error", fontsize=7)
    ax.set_ylim(0, bound * 1.6)
    ax.ticklabel_format(axis="y", style="scientific", scilimits=(0, 0))

    # ====================================================================
    # (b) NRMSE
    # ====================================================================
    ax = axes[1]
    style.panel_label(ax, "(b)")

    nrmse = rzfp["nrmse"].values.astype(float)

    bars = ax.bar(x, nrmse, width=0.45, color=style.COLORS["rzfp"],
                  edgecolor="#333333", linewidth=0.5)

    # Value labels
    for i, (v, vi) in enumerate(zip(nrmse, violations)):
        ax.text(i, v * 1.4, f"{v:.2e}\n({vi} violations)",
                ha="center", va="bottom", fontsize=5.5)

    ax.set_xticks(x)
    ax.set_xticklabels(datasets, fontsize=6)
    ax.set_ylabel("NRMSE", fontsize=7)
    ax.ticklabel_format(axis="y", style="scientific", scilimits=(0, 0))

    # Annotation
    ax.text(0.98, 0.95, "Low NRMSE despite\nnear-bound max error:\noverall accuracy is high",
            transform=ax.transAxes, ha="right", va="top", fontsize=4.5,
            fontstyle="italic", color="#777777",
            bbox=dict(boxstyle="round,pad=0.3", facecolor="#F8F8F8",
                     edgecolor="#CCCCCC", linewidth=0.3))

    fig.subplots_adjust(wspace=0.4)
    style.savefig(fig, "Fig6_RZFP_accuracy", outdir)
    plt.close(fig)

if __name__ == "__main__":
    make_figure()
