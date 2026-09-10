"""Fig3: 20GB multi-axis access performance.

(a) Random access: Raw vs ERWT3D (X/Y only, Z excluded as cache-sensitive)
(b) Continuous access: Raw vs ERWT3D (X/Y only)
(c) ERWT3D axis balance: X/Y/Z random & continuous

Source: validation/summaries_final/benchmark_aggregate.csv
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
    files = glob.glob(os.path.join(RAW_DIR, subdir,
        f"*{dataset}*{configuration}*{device}*{axis}_{pattern}_run*.json"))
    times = []
    for f in sorted(files):
        with open(f) as fh:
            d = json.load(fh)
            if d.get("format") == format_ and d.get("status") == "SUCCESS":
                times.append(d["total_time_ms"] / 1000.0)
    return times

def plot_comparison(ax, raw_data, erwt3d_data, axes_to_plot, title):
    """Plot Raw vs ERWT3D comparison with individual points + mean ± SD."""
    x_pos = np.arange(len(axes_to_plot))
    width = 0.3

    for i, ax_name in enumerate(axes_to_plot):
        # Raw
        raw_vals = raw_data.get(ax_name, [])
        if raw_vals:
            ax.scatter(np.full(len(raw_vals), i - width/2), raw_vals,
                      color=style.COLORS["raw"], marker=style.MARKERS["raw"],
                      s=12, alpha=0.5, zorder=3)
            mean_r = np.mean(raw_vals)
            std_r = np.std(raw_vals)
            ax.errorbar(i - width/2, mean_r, yerr=std_r, color=style.COLORS["raw"],
                       fmt=style.MARKERS["raw"], markersize=5, capsize=3, linewidth=1.0,
                       markeredgecolor="#333333", markeredgewidth=0.5)

        # ERWT3D
        erwt_vals = erwt3d_data.get(ax_name, [])
        if erwt_vals:
            ax.scatter(np.full(len(erwt_vals), i + width/2), erwt_vals,
                      color=style.COLORS["erwt3d"], marker=style.MARKERS["erwt3d"],
                      s=12, alpha=0.5, zorder=3)
            mean_e = np.mean(erwt_vals)
            std_e = np.std(erwt_vals)
            ax.errorbar(i + width/2, mean_e, yerr=std_e, color=style.COLORS["erwt3d"],
                       fmt=style.MARKERS["erwt3d"], markersize=5, capsize=3, linewidth=1.0,
                       markeredgecolor="#333333", markeredgewidth=0.5)

    ax.set_xticks(x_pos)
    ax.set_xticklabels(axes_to_plot, fontsize=6)
    ax.set_ylabel("Access time (s)", fontsize=7)
    ax.set_title(title, fontsize=7, fontweight="bold")

def make_figure(outdir="paper_figures/output"):
    fig, axes = plt.subplots(1, 3, figsize=(style.WIDTH_DOUBLE, 60/25.4))

    # Load data
    raw_random = {ax: load_per_run("20GB", "raw_baseline", "RAW", "SSD", ax, "random") for ax in ["x", "y"]}
    erwt3d_random = {ax: load_per_run("20GB", "A0_auto_final", "LZ4", "SSD", ax, "random") for ax in ["x", "y"]}
    raw_cont = {ax: load_per_run("20GB", "raw_baseline", "RAW", "SSD", ax, "continuous") for ax in ["x", "y"]}
    erwt3d_cont = {ax: load_per_run("20GB", "A0_auto_final", "LZ4", "SSD", ax, "continuous") for ax in ["x", "y"]}

    erwt3d_all_random = {ax: load_per_run("20GB", "A0_auto_final", "LZ4", "SSD", ax, "random") for ax in ["x", "y", "z"]}
    erwt3d_all_cont = {ax: load_per_run("20GB", "A0_auto_final", "LZ4", "SSD", ax, "continuous") for ax in ["x", "y", "z"]}

    # --- (a) Random access ---
    ax = axes[0]
    style.panel_label(ax, "(a)")
    plot_comparison(ax, raw_random, erwt3d_random, ["x", "y"], "Random access")
    # Z annotation
    yl = ax.get_ylim()
    ax.text(2, yl[1] * 0.55, "Z raw:\ncache-sensitive\n(excluded)",
            ha="center", va="center", fontsize=5, fontstyle="italic",
            color=style.COLORS["raw"],
            bbox=dict(boxstyle="round,pad=0.3", facecolor=style.COLORS["bg"],
                     edgecolor=style.COLORS["raw"], linewidth=0.4))
    ax.legend(["Raw", "ERWT3D"], loc="upper left", fontsize=5.5)

    # --- (b) Continuous access ---
    ax = axes[1]
    style.panel_label(ax, "(b)")
    plot_comparison(ax, raw_cont, erwt3d_cont, ["x", "y"], "Continuous access")
    yl = ax.get_ylim()
    ax.text(2, yl[1] * 0.55, "Z raw:\ncache-sensitive\n(excluded)",
            ha="center", va="center", fontsize=5, fontstyle="italic",
            color=style.COLORS["raw"],
            bbox=dict(boxstyle="round,pad=0.3", facecolor=style.COLORS["bg"],
                     edgecolor=style.COLORS["raw"], linewidth=0.4))

    # --- (c) ERWT3D axis balance ---
    ax = axes[2]
    style.panel_label(ax, "(c)")
    ax.set_title("ERWT3D axis balance", fontsize=7, fontweight="bold")

    x_pos = np.arange(3)
    axes_names = ["x", "y", "z"]

    for j, (pat, data_dict, color, marker) in enumerate([
        ("Random", erwt3d_all_random, style.COLORS["erwt3d"], "o"),
        ("Continuous", erwt3d_all_cont, style.COLORS["aux"], "^"),
    ]):
        means = []
        stds = []
        for ax_name in axes_names:
            vals = data_dict.get(ax_name, [])
            means.append(np.mean(vals) if vals else 0)
            stds.append(np.std(vals) if vals else 0)

        offset = (j - 0.5) * 0.2
        ax.errorbar(x_pos + offset, means, yerr=stds, color=color,
                   fmt=marker, markersize=5, capsize=3, linewidth=1.0,
                   markeredgecolor="#333333", markeredgewidth=0.5, label=pat)
        ax.plot(x_pos + offset, means, color=color, linewidth=0.8, alpha=0.4)

    ax.set_xticks(x_pos)
    ax.set_xticklabels(["X", "Y", "Z"], fontsize=6)
    ax.set_ylabel("Access time (s)", fontsize=7)
    ax.legend(fontsize=5.5, loc="upper left")

    # Annotation
    ax.text(0.98, 0.05, "ERWT3D provides\ncomparable access\nacross all axes",
            transform=ax.transAxes, ha="right", va="bottom", fontsize=4.5,
            fontstyle="italic", color="#777777",
            bbox=dict(boxstyle="round,pad=0.3", facecolor="#F8F8F8",
                     edgecolor="#CCCCCC", linewidth=0.3))

    fig.subplots_adjust(wspace=0.35)
    style.savefig(fig, "Fig3_multiaxis_access", outdir)
    plt.close(fig)

if __name__ == "__main__":
    make_figure()
