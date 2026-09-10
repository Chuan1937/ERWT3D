"""Fig4: Codec ablation on 20GB.

(a) Physical storage ratio — Auto LZ4 vs Forced LZ4 vs Forced RZFP
(b) Composite access time — individual runs + mean ± SD
(c) Random access by axis: LZ4 vs RZFP

Core message: LZ4 provides lower storage, lossless, and faster access.
Auto and Forced LZ4 confirm planner consistency.

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

def load_composite_runs(configuration, format_):
    """Load per-run composite time (mean of 6 workloads)."""
    times_by_run = {}
    for axis in ["x", "y", "z"]:
        for pattern in ["random", "continuous"]:
            subdir = "random_read" if pattern == "random" else "continuous_read"
            files = glob.glob(os.path.join(RAW_DIR, subdir,
                f"*20GB*{configuration}*SSD*{axis}_{pattern}_run*.json"))
            for f in sorted(files):
                with open(f) as fh:
                    d = json.load(fh)
                    if d.get("format") == format_ and d.get("status") == "SUCCESS":
                        run = d.get("run_number", 0)
                        if run not in times_by_run:
                            times_by_run[run] = []
                        times_by_run[run].append(d["total_time_ms"] / 1000.0)
    composites = []
    for run in sorted(times_by_run.keys()):
        if len(times_by_run[run]) == 6:
            composites.append(np.mean(times_by_run[run]))
    return composites

def make_figure(outdir="paper_figures/output"):
    agg = pd.read_csv(os.path.join(DATA_DIR, "benchmark_aggregate.csv"))

    fig, axes = plt.subplots(1, 3, figsize=(style.WIDTH_DOUBLE, 55/25.4))

    # Config info: (label, format, color, alpha, hatch)
    configs = [
        ("Auto LZ4", "A0_auto_final", "LZ4", style.COLORS["lz4"], 1.0, ""),
        ("Forced LZ4", "A1_force_lz4_final", "LZ4", style.COLORS["lz4"], 0.5, "//"),
        ("Forced RZFP", "A1_force_rzfp_final", "RZFP", style.COLORS["rzfp"], 1.0, ""),
    ]

    # ====================================================================
    # (a) Physical storage ratio
    # ====================================================================
    ax = axes[0]
    style.panel_label(ax, "(a)")

    ratios = [1.012, 1.012, 1.768]
    labels = [c[0] for c in configs]
    colors = [c[3] for c in configs]
    alphas = [c[4] for c in configs]

    x = np.arange(len(configs))
    for i, (r, c, a) in enumerate(zip(ratios, colors, alphas)):
        ax.bar(i, r, width=0.5, color=c, alpha=a, edgecolor="#333333", linewidth=0.5)
        ax.text(i, r + 0.05, f"{r:.3f}×", ha="center", va="bottom", fontsize=5.5)

    ax.axhline(y=1.0, color=style.COLORS["raw"], linestyle="--", linewidth=0.8, label="Raw")
    ax.set_xticks(x)
    ax.set_xticklabels(labels, fontsize=5.5, rotation=15, ha="right")
    ax.set_ylabel("Physical storage ratio", fontsize=7)
    ax.set_ylim(0, 2.2)
    ax.legend(fontsize=5.5)

    # ====================================================================
    # (b) Composite access time
    # ====================================================================
    ax = axes[1]
    style.panel_label(ax, "(b)")

    composites = {}
    for label, cfg, fmt, color, alpha, hatch in configs:
        composites[label] = load_composite_runs(cfg, fmt)

    for i, (label, _, _, color, alpha, _) in enumerate(configs):
        vals = composites[label]
        if vals:
            ax.scatter(np.full(len(vals), i), vals, color=color, marker="o",
                      s=15, alpha=0.5 * alpha + 0.3, zorder=3)
            mean_v = np.mean(vals)
            std_v = np.std(vals)
            ax.errorbar(i, mean_v, yerr=std_v, color=color, fmt="o",
                       markersize=5, capsize=3, linewidth=1.0,
                       markeredgecolor="#333333", markeredgewidth=0.5, alpha=alpha)

    ax.set_xticks(np.arange(len(labels)))
    ax.set_xticklabels(labels, fontsize=5.5, rotation=15, ha="right")
    ax.set_ylabel("Composite access time (s)", fontsize=7)

    # Annotation
    ax.text(0.98, 0.95, "Auto ≈ Forced LZ4\n(same representation)",
            transform=ax.transAxes, ha="right", va="top", fontsize=4.5,
            fontstyle="italic", color="#777777")

    # ====================================================================
    # (c) Random access by axis: LZ4 vs RZFP
    # ====================================================================
    ax = axes[2]
    style.panel_label(ax, "(c)")

    axes_names = ["x", "y", "z"]
    x_pos = np.arange(3)

    # LZ4 (use Auto as representative)
    lz4_means = []
    lz4_stds = []
    for ax_name in axes_names:
        row = agg[(agg["dataset"] == "20GB") & (agg["configuration"] == "A0_auto_final") &
                 (agg["format"] == "LZ4") & (agg["device"] == "SSD") &
                 (agg["axis"] == ax_name) & (agg["pattern"] == "random")]
        if not row.empty:
            lz4_means.append(row.iloc[0]["mean_ms"] / 1000.0)
            lz4_stds.append(row.iloc[0]["std_ms"] / 1000.0)
        else:
            lz4_means.append(0)
            lz4_stds.append(0)

    # RZFP
    rzfp_means = []
    rzfp_stds = []
    for ax_name in axes_names:
        row = agg[(agg["dataset"] == "20GB") & (agg["configuration"] == "A1_force_rzfp_final") &
                 (agg["format"] == "RZFP") & (agg["device"] == "SSD") &
                 (agg["axis"] == ax_name) & (agg["pattern"] == "random")]
        if not row.empty:
            rzfp_means.append(row.iloc[0]["mean_ms"] / 1000.0)
            rzfp_stds.append(row.iloc[0]["std_ms"] / 1000.0)
        else:
            rzfp_means.append(0)
            rzfp_stds.append(0)

    width = 0.25
    ax.errorbar(x_pos - width/2, lz4_means, yerr=lz4_stds, color=style.COLORS["lz4"],
               fmt="o", markersize=5, capsize=3, linewidth=1.0,
               markeredgecolor="#333333", markeredgewidth=0.5, label="LZ4")
    ax.plot(x_pos - width/2, lz4_means, color=style.COLORS["lz4"], linewidth=0.8, alpha=0.4)

    ax.errorbar(x_pos + width/2, rzfp_means, yerr=rzfp_stds, color=style.COLORS["rzfp"],
               fmt="D", markersize=5, capsize=3, linewidth=1.0,
               markeredgecolor="#333333", markeredgewidth=0.5, label="RZFP")
    ax.plot(x_pos + width/2, rzfp_means, color=style.COLORS["rzfp"], linewidth=0.8, alpha=0.4)

    ax.set_xticks(x_pos)
    ax.set_xticklabels(["X", "Y", "Z"], fontsize=6)
    ax.set_ylabel("Random access time (s)", fontsize=7)
    ax.legend(fontsize=5.5, loc="upper left")

    fig.subplots_adjust(wspace=0.35)
    style.savefig(fig, "Fig4_codec_ablation", outdir)
    plt.close(fig)

if __name__ == "__main__":
    make_figure()
