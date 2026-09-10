"""Fig3: 20GB multi-axis access performance.

Source: validation/summaries_final/benchmark_aggregate.csv
        validation/summaries_final/variability.csv
        /mnt/f/CUP/results/raw_results_final/ (per-run JSONs)
"""
import sys, os, json, glob
sys.path.insert(0, os.path.dirname(__file__))
import style
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

DATA_DIR = os.path.join(os.path.dirname(__file__), "..", "..", "validation", "summaries_final")
RAW_DIR = "/mnt/f/CUP/results/raw_results_final"

def load_per_run(dataset, configuration, format_, device, axis, pattern):
    """Load per-run total_time_ms from raw JSON files."""
    subdir = "random_read" if pattern == "random" else "continuous_read"
    pattern_files = glob.glob(os.path.join(RAW_DIR, subdir, f"*{dataset}*{configuration}*{device}*{axis}_{pattern}_run*.json"))
    times = []
    for f in sorted(pattern_files):
        with open(f) as fh:
            d = json.load(fh)
            if d.get("format") == format_ and d.get("status") == "SUCCESS":
                times.append(d["total_time_ms"] / 1000.0)  # Convert to seconds
    return times

def make_figure(outdir="paper_figures/output"):
    agg = pd.read_csv(os.path.join(DATA_DIR, "benchmark_aggregate.csv"))

    fig, axes = plt.subplots(1, 3, figsize=(style.WIDTH_DOUBLE, 65/25.4))

    axes_labels = ["x", "y", "z"]
    patterns = ["random", "continuous"]
    devices = ["SSD"]

    # --- (a) Random access: Raw vs ERWT3D ---
    ax = axes[0]
    style.panel_label(ax, "(a)")
    ax.set_title("Random access", fontsize=7, fontweight="bold")

    raw_times = {}
    erwt3d_times = {}
    for ax_name in ["x", "y"]:
        raw_times[ax_name] = load_per_run("20GB", "raw_baseline", "RAW", "SSD", ax_name, "random")
        erwt3d_times[ax_name] = load_per_run("20GB", "A0_auto_final", "LZ4", "SSD", ax_name, "random")

    x_pos = np.arange(2)
    width = 0.3

    # Plot individual points + mean ± SD
    for i, ax_name in enumerate(["x", "y"]):
        # Raw
        raw_vals = raw_times[ax_name]
        if raw_vals:
            ax.scatter(np.full(len(raw_vals), i - width/2), raw_vals,
                      color=style.COLORS["raw"], marker=style.MARKERS["raw"],
                      s=15, alpha=0.6, zorder=3)
            mean_r = np.mean(raw_vals)
            std_r = np.std(raw_vals)
            ax.errorbar(i - width/2, mean_r, yerr=std_r, color=style.COLORS["raw"],
                       fmt=style.MARKERS["raw"], markersize=5, capsize=3, linewidth=1.0,
                       markeredgecolor="#333333", markeredgewidth=0.5)

        # ERWT3D
        erwt_vals = erwt3d_times[ax_name]
        if erwt_vals:
            ax.scatter(np.full(len(erwt_vals), i + width/2), erwt_vals,
                      color=style.COLORS["erwt3d"], marker=style.MARKERS["erwt3d"],
                      s=15, alpha=0.6, zorder=3)
            mean_e = np.mean(erwt_vals)
            std_e = np.std(erwt_vals)
            ax.errorbar(i + width/2, mean_e, yerr=std_e, color=style.COLORS["erwt3d"],
                       fmt=style.MARKERS["erwt3d"], markersize=5, capsize=3, linewidth=1.0,
                       markeredgecolor="#333333", markeredgewidth=0.5)

    # Z excluded annotation
    ax.text(2, ax.get_ylim()[1] * 0.5, "Z raw:\ncache-\nsensitive",
            ha="center", va="center", fontsize=5.5, fontstyle="italic",
            color=style.COLORS["raw"], bbox=dict(boxstyle="round,pad=0.3",
            facecolor=style.COLORS["bg"], edgecolor=style.COLORS["raw"], linewidth=0.5))

    ax.set_xticks([0, 1, 2])
    ax.set_xticklabels(["X", "Y", "Z"], fontsize=6)
    ax.set_ylabel("Access time (s)", fontsize=7)
    ax.legend(["Raw", "ERWT3D"], loc="upper left", fontsize=5.5)

    # --- (b) Continuous access ---
    ax = axes[1]
    style.panel_label(ax, "(b)")
    ax.set_title("Continuous access", fontsize=7, fontweight="bold")

    raw_times_c = {}
    erwt3d_times_c = {}
    for ax_name in ["x", "y"]:
        raw_times_c[ax_name] = load_per_run("20GB", "raw_baseline", "RAW", "SSD", ax_name, "continuous")
        erwt3d_times_c[ax_name] = load_per_run("20GB", "A0_auto_final", "LZ4", "SSD", ax_name, "continuous")

    for i, ax_name in enumerate(["x", "y"]):
        raw_vals = raw_times_c[ax_name]
        if raw_vals:
            ax.scatter(np.full(len(raw_vals), i - width/2), raw_vals,
                      color=style.COLORS["raw"], marker=style.MARKERS["raw"],
                      s=15, alpha=0.6, zorder=3)
            mean_r = np.mean(raw_vals)
            std_r = np.std(raw_vals)
            ax.errorbar(i - width/2, mean_r, yerr=std_r, color=style.COLORS["raw"],
                       fmt=style.MARKERS["raw"], markersize=5, capsize=3, linewidth=1.0,
                       markeredgecolor="#333333", markeredgewidth=0.5)

        erwt_vals = erwt3d_times_c[ax_name]
        if erwt_vals:
            ax.scatter(np.full(len(erwt_vals), i + width/2), erwt_vals,
                      color=style.COLORS["erwt3d"], marker=style.MARKERS["erwt3d"],
                      s=15, alpha=0.6, zorder=3)
            mean_e = np.mean(erwt_vals)
            std_e = np.std(erwt_vals)
            ax.errorbar(i + width/2, mean_e, yerr=std_e, color=style.COLORS["erwt3d"],
                       fmt=style.MARKERS["erwt3d"], markersize=5, capsize=3, linewidth=1.0,
                       markeredgecolor="#333333", markeredgewidth=0.5)

    ax.text(2, ax.get_ylim()[1] * 0.5, "Z raw:\ncache-\nsensitive",
            ha="center", va="center", fontsize=5.5, fontstyle="italic",
            color=style.COLORS["raw"], bbox=dict(boxstyle="round,pad=0.3",
            facecolor=style.COLORS["bg"], edgecolor=style.COLORS["raw"], linewidth=0.5))

    ax.set_xticks([0, 1, 2])
    ax.set_xticklabels(["X", "Y", "Z"], fontsize=6)
    ax.set_ylabel("Access time (s)", fontsize=7)

    # --- (c) ERWT3D XYZ balance ---
    ax = axes[2]
    style.panel_label(ax, "(c)")
    ax.set_title("ERWT3D axis balance", fontsize=7, fontweight="bold")

    erwt3d_data = {}
    for ax_name in ["x", "y", "z"]:
        for pat in ["random", "continuous"]:
            erwt3d_data[(ax_name, pat)] = load_per_run("20GB", "A0_auto_final", "LZ4", "SSD", ax_name, pat)

    x_pos = np.arange(3)
    for j, pat in enumerate(["random", "continuous"]):
        means = []
        stds = []
        for ax_name in ["x", "y", "z"]:
            vals = erwt3d_data[(ax_name, pat)]
            means.append(np.mean(vals) if vals else 0)
            stds.append(np.std(vals) if vals else 0)

        offset = (j - 0.5) * 0.2
        color = style.COLORS["erwt3d"] if pat == "random" else style.COLORS["aux"]
        marker = "o" if pat == "random" else "^"

        ax.errorbar(x_pos + offset, means, yerr=stds, color=color,
                   fmt=marker, markersize=5, capsize=3, linewidth=1.0,
                   markeredgecolor="#333333", markeredgewidth=0.5, label=pat.capitalize())

        # Connect with line
        ax.plot(x_pos + offset, means, color=color, linewidth=0.8, alpha=0.5)

    ax.set_xticks(x_pos)
    ax.set_xticklabels(["X", "Y", "Z"], fontsize=6)
    ax.set_ylabel("Access time (s)", fontsize=7)
    ax.legend(loc="upper left", fontsize=5.5)

    fig.subplots_adjust(wspace=0.35)
    style.savefig(fig, "Fig3_multiaxis_access", outdir)
    plt.close(fig)

if __name__ == "__main__":
    make_figure()
