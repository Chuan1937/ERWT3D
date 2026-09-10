"""Fig2: Adaptive selection and physical storage.

(a) Physical storage ratio across datasets
(b) Planner prediction vs measured for 20GB

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

    # ====================================================================
    # (a) Physical storage ratio across datasets
    # ====================================================================
    ax = axes[0]
    style.panel_label(ax, "(a)")

    datasets = ds["dataset"].values
    ratios = ds["physical_storage_ratio"].values.astype(float)
    formats = ds["auto_format"].values

    colors = []
    labels = []
    for fmt in formats:
        if "LZ4" in fmt:
            colors.append(style.COLORS["lz4"])
            labels.append("LZ4")
        elif "RZFP" in fmt:
            colors.append(style.COLORS["rzfp"])
            labels.append("RZFP")

    x = np.arange(len(datasets))
    bars = ax.bar(x, ratios, width=0.5, color=colors, edgecolor="#333333", linewidth=0.5)

    # Raw baseline
    ax.axhline(y=1.0, color=style.COLORS["raw"], linestyle="--", linewidth=0.8, label="Raw baseline")

    # Labels on bars
    for i, (r, fmt_short) in enumerate(zip(ratios, labels)):
        ax.text(i, r + 0.12, f"{r:.3f}×\n({fmt_short})", ha="center", va="bottom",
                fontsize=5.5, fontweight="bold")

    # Note for F3 amplitude high ratio
    ax.text(0, ratios[0] + 0.7,
            "Small volume:\nfixed overhead\ndominates",
            ha="center", va="bottom", fontsize=4.5, fontstyle="italic",
            color="#777777",
            bbox=dict(boxstyle="round,pad=0.2", facecolor="#F8F8F8",
                     edgecolor="#CCCCCC", linewidth=0.3))

    ax.set_xticks(x)
    ax.set_xticklabels(datasets, fontsize=6)
    ax.set_ylabel("Physical storage ratio", fontsize=7)
    ax.set_ylim(0, max(ratios) * 1.5)
    ax.legend(loc="upper right", fontsize=5.5)

    # ====================================================================
    # (b) Planner prediction vs measured for 20GB
    # ====================================================================
    ax = axes[1]
    style.panel_label(ax, "(b)")

    # Planner predictions from erwt3d_auto_plan output
    planner = {
        "LZ4+XYZ":  {"mean": 2.142, "upper": 2.937, "measured": 1.012, "selected": True},
        "RZFP+XYZ": {"mean": 0.840, "upper": 0.841, "measured": 1.768, "selected": False},
    }

    configs = list(planner.keys())
    x2 = np.arange(len(configs))
    width = 0.28

    for i, (cfg, vals) in enumerate(planner.items()):
        color = style.COLORS["lz4"] if "LZ4" in cfg else style.COLORS["rzfp"]

        # Predicted mean (circle)
        ax.plot(i - width, vals["mean"], "o", color=color, markersize=6,
               markeredgecolor="#333333", markeredgewidth=0.5, zorder=3,
               label="Predicted" if i == 0 else "")
        # Predicted upper (cap)
        ax.plot([i - width, i - width], [vals["mean"], vals["upper"]],
               color="#333333", linewidth=0.8, zorder=2)
        ax.plot(i - width, vals["upper"], "_", color="#333333", markersize=8,
               markeredgewidth=0.8, zorder=2)

        # Measured (diamond)
        ax.plot(i + width, vals["measured"], "D", color=color, markersize=6,
               markeredgecolor="#333333", markeredgewidth=0.5, zorder=3,
               label="Measured" if i == 0 else "")

        # Selection annotation
        if vals["selected"]:
            ax.annotate("Selected", xy=(i + width, vals["measured"]),
                       xytext=(i + width, vals["measured"] + 0.5),
                       ha="center", fontsize=5.5, fontweight="bold", color=color,
                       arrowprops=dict(arrowstyle="-|>", color=color, lw=0.8))

    # Raw baseline
    ax.axhline(y=1.0, color=style.COLORS["raw"], linestyle="--", linewidth=0.8)

    ax.set_xticks(x2)
    ax.set_xticklabels(configs, fontsize=6)
    ax.set_ylabel("Physical storage ratio", fontsize=7)
    ax.set_ylim(0, 3.5)
    ax.legend(fontsize=5.5, loc="upper left")

    # Annotation: predicted vs measured gap
    ax.text(0.98, 0.05, "Predicted ≠ Measured:\nembedded sections reduce\nLZ4 overhead in practice",
            transform=ax.transAxes, ha="right", va="bottom", fontsize=4.5,
            fontstyle="italic", color="#777777",
            bbox=dict(boxstyle="round,pad=0.3", facecolor="#F8F8F8",
                     edgecolor="#CCCCCC", linewidth=0.3))

    fig.subplots_adjust(wspace=0.35)
    style.savefig(fig, "Fig2_storage_selection", outdir)
    plt.close(fig)

if __name__ == "__main__":
    make_figure()
