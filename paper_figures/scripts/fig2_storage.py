"""Fig2: Adaptive representation and storage.

Source: validation/summaries_final/dataset_summary.csv
"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
import style
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

DATA_DIR = os.path.join(os.path.dirname(__file__), "..", "..", "validation", "summaries_final")

def make_figure(outdir="paper_figures/output"):
    ds = pd.read_csv(os.path.join(DATA_DIR, "dataset_summary.csv"))

    fig, axes = plt.subplots(1, 2, figsize=(style.WIDTH_DOUBLE, 55/25.4))

    # --- (a) Dataset physical storage ratio ---
    ax = axes[0]
    style.panel_label(ax, "(a)")

    datasets = ds["dataset"].values
    ratios = ds["physical_storage_ratio"].values.astype(float)
    formats = ds["auto_format"].values

    colors = []
    for fmt in formats:
        if "LZ4" in fmt:
            colors.append(style.COLORS["lz4"])
        elif "RZFP" in fmt:
            colors.append(style.COLORS["rzfp"])
        else:
            colors.append(style.COLORS["raw"])

    x = np.arange(len(datasets))
    bars = ax.bar(x, ratios, width=0.55, color=colors, edgecolor="#333333", linewidth=0.5)

    # Raw baseline
    ax.axhline(y=1.0, color=style.COLORS["raw"], linestyle="--", linewidth=0.8, label="Raw baseline")

    # Labels on bars
    for i, (r, fmt) in enumerate(zip(ratios, formats)):
        fmt_short = fmt.replace("+XYZ", "")
        ax.text(i, r + 0.08, f"{r:.3f}×\n({fmt_short})", ha="center", va="bottom",
                fontsize=5.5, fontweight="bold")

    ax.set_xticks(x)
    ax.set_xticklabels(datasets, fontsize=6)
    ax.set_ylabel("Physical storage ratio", fontsize=7)
    ax.set_ylim(0, max(ratios) * 1.35)
    ax.legend(loc="upper left", fontsize=5.5)

    # --- (b) 20GB planner prediction vs measured ---
    ax = axes[1]
    style.panel_label(ax, "(b)")

    # Planner predictions (from auto_plan output)
    # These are the predicted values from the planner
    planner_lz4_mean = 1.468
    planner_lz4_upper = 1.548
    planner_rzfp_mean = 0.840
    planner_rzfp_upper = 0.841

    # Measured from dataset_summary
    row_20gb = ds[ds["dataset"] == "20GB"].iloc[0]
    measured_lz4 = row_20gb["physical_storage_ratio"]

    # For RZFP, we know from the converter output
    measured_rzfp = 1.768  # From 34068786624 / 19271755620

    configs = ["LZ4+XYZ", "RZFP+XYZ"]
    predicted_mean = [planner_lz4_mean, planner_rzfp_mean]
    predicted_upper = [planner_lz4_upper, planner_rzfp_upper]
    measured = [measured_lz4, measured_rzfp]

    x2 = np.arange(len(configs))
    width = 0.3

    # Predicted bars with error caps
    bars_pred = ax.bar(x2 - width/2, predicted_mean, width,
                       color=[style.COLORS["lz4"], style.COLORS["rzfp"]],
                       alpha=0.4, edgecolor="#333333", linewidth=0.5, label="Predicted")
    # Upper bound caps
    for i, (pm, pu) in enumerate(zip(predicted_mean, predicted_upper)):
        ax.plot([i - width/2, i - width/2], [pm, pu], color="#333333", linewidth=0.8)
        ax.plot(i - width/2, pu, "_", color="#333333", markersize=6, markeredgewidth=0.8)

    # Measured bars
    bars_meas = ax.bar(x2 + width/2, measured, width,
                       color=[style.COLORS["lz4"], style.COLORS["rzfp"]],
                       edgecolor="#333333", linewidth=0.5, label="Measured")

    # Selection annotation
    ax.annotate("Selected", xy=(0 + width/2, measured_lz4),
                xytext=(0 + width/2, measured_lz4 + 0.25),
                ha="center", fontsize=5.5, fontweight="bold", color=style.COLORS["lz4"],
                arrowprops=dict(arrowstyle="-|>", color=style.COLORS["lz4"], lw=0.8))

    # Raw baseline
    ax.axhline(y=1.0, color=style.COLORS["raw"], linestyle="--", linewidth=0.8)

    ax.set_xticks(x2)
    ax.set_xticklabels(configs, fontsize=6)
    ax.set_ylabel("Physical storage ratio", fontsize=7)
    ax.set_ylim(0, 2.2)
    ax.legend(loc="upper left", fontsize=5.5, ncol=2)

    fig.subplots_adjust(wspace=0.35)
    style.savefig(fig, "Fig2_storage_selection", outdir)
    plt.close(fig)

if __name__ == "__main__":
    make_figure()
