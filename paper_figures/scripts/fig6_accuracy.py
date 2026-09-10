"""Fig6: RZFP error-bound verification.

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

    fig, axes = plt.subplots(1, 2, figsize=(style.WIDTH_SINGLE * 2, 50/25.4))

    # Filter RZFP records
    rzfp = acc[acc["format"] == "RZFP"].copy()

    # --- (a) Maximum relative error ---
    ax = axes[0]
    style.panel_label(ax, "(a)")

    datasets = rzfp["dataset"].values
    max_rel = rzfp["max_relative_error"].values.astype(float)

    x = np.arange(len(datasets))
    bars = ax.bar(x, max_rel, width=0.5, color=[style.COLORS["rzfp"]] * len(datasets),
                  edgecolor="#333333", linewidth=0.5)

    # Error bound line
    bound = 1e-3
    ax.axhline(y=bound, color="#333333", linestyle="--", linewidth=0.8)
    ax.text(len(datasets) - 0.5, bound * 1.1, f"Error bound\n({bound:.0e})",
            ha="right", va="bottom", fontsize=5.5, fontstyle="italic")

    # Value labels
    for i, v in enumerate(max_rel):
        ax.text(i, v + bound * 0.05, f"{v:.2e}", ha="center", va="bottom",
                fontsize=5.5, fontweight="bold")

    ax.set_xticks(x)
    ax.set_xticklabels(datasets, fontsize=6)
    ax.set_ylabel("Max relative error", fontsize=7)
    ax.set_ylim(0, bound * 1.5)
    ax.ticklabel_format(axis="y", style="scientific", scilimits=(0, 0))

    # --- (b) NRMSE ---
    ax = axes[1]
    style.panel_label(ax, "(b)")

    nrmse = rzfp["nrmse"].values.astype(float)
    violations = rzfp["violation_count"].values.astype(int)

    bars = ax.bar(x, nrmse, width=0.5, color=[style.COLORS["rzfp"]] * len(datasets),
                  edgecolor="#333333", linewidth=0.5)

    # Value labels with violations
    for i, (v, vi) in enumerate(zip(nrmse, violations)):
        ax.text(i, v * 1.3, f"{v:.2e}\nViolations: {vi}", ha="center", va="bottom",
                fontsize=5.5)

    ax.set_xticks(x)
    ax.set_xticklabels(datasets, fontsize=6)
    ax.set_ylabel("NRMSE", fontsize=7)
    ax.ticklabel_format(axis="y", style="scientific", scilimits=(0, 0))

    fig.subplots_adjust(wspace=0.4)
    style.savefig(fig, "Fig6_RZFP_accuracy", outdir)
    plt.close(fig)

if __name__ == "__main__":
    make_figure()
