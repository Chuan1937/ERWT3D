"""Fig4: Codec ablation.

Source: validation/summaries_final/benchmark_aggregate.csv
        validation/summaries_final/dataset_summary.csv
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
    """Load per-run composite time across all 6 workloads."""
    times_by_run = {}
    for axis in ["x", "y", "z"]:
        for pattern in ["random", "continuous"]:
            subdir = "random_read" if pattern == "random" else "continuous_read"
            files = glob.glob(os.path.join(RAW_DIR, subdir, f"*20GB*{configuration}*SSD*{axis}_{pattern}_run*.json"))
            for f in sorted(files):
                with open(f) as fh:
                    d = json.load(fh)
                    if d.get("format") == format_ and d.get("status") == "SUCCESS":
                        run = d.get("run_number", 0)
                        if run not in times_by_run:
                            times_by_run[run] = []
                        times_by_run[run].append(d["total_time_ms"] / 1000.0)
    # Compute composite per run
    composites = []
    for run in sorted(times_by_run.keys()):
        if len(times_by_run[run]) == 6:
            composites.append(np.mean(times_by_run[run]))
    return composites

def make_figure(outdir="paper_figures/output"):
    ds = pd.read_csv(os.path.join(DATA_DIR, "dataset_summary.csv"))
    agg = pd.read_csv(os.path.join(DATA_DIR, "benchmark_aggregate.csv"))

    fig, axes = plt.subplots(1, 3, figsize=(style.WIDTH_DOUBLE, 55/25.4))

    # --- (a) Physical storage ratio ---
    ax = axes[0]
    style.panel_label(ax, "(a)")

    configs = ["Auto LZ4", "Forced LZ4", "Forced RZFP"]
    ratios = [1.012, 1.012, 1.768]  # From dataset_summary / converter output
    colors = [style.COLORS["lz4"], style.COLORS["lz4"], style.COLORS["rzfp"]]
    alphas = [1.0, 0.6, 1.0]

    x = np.arange(len(configs))
    for i, (r, c, a) in enumerate(zip(ratios, colors, alphas)):
        ax.bar(i, r, width=0.55, color=c, alpha=a, edgecolor="#333333", linewidth=0.5)
        ax.text(i, r + 0.05, f"{r:.3f}×", ha="center", va="bottom", fontsize=5.5)

    ax.axhline(y=1.0, color=style.COLORS["raw"], linestyle="--", linewidth=0.8, label="Raw")
    ax.set_xticks(x)
    ax.set_xticklabels(configs, fontsize=5.5, rotation=15)
    ax.set_ylabel("Physical storage ratio", fontsize=7)
    ax.set_ylim(0, 2.2)
    ax.legend(fontsize=5.5)

    # --- (b) Composite access time ---
    ax = axes[1]
    style.panel_label(ax, "(b)")

    composites = {
        "Auto LZ4": load_composite_runs("A0_auto_final", "LZ4"),
        "Forced LZ4": load_composite_runs("A1_force_lz4_final", "LZ4"),
        "Forced RZFP": load_composite_runs("A1_force_rzfp_final", "RZFP"),
    }

    for i, (cfg, vals) in enumerate(composites.items()):
        if vals:
            ax.scatter(np.full(len(vals), i), vals, color=colors[i], marker="o",
                      s=20, alpha=0.6, zorder=3)
            mean_v = np.mean(vals)
            std_v = np.std(vals)
            ax.errorbar(i, mean_v, yerr=std_v, color=colors[i], fmt="o",
                       markersize=5, capsize=3, linewidth=1.0,
                       markeredgecolor="#333333", markeredgewidth=0.5)

    ax.set_xticks(np.arange(len(configs)))
    ax.set_xticklabels(configs, fontsize=5.5, rotation=15)
    ax.set_ylabel("Composite access time (s)", fontsize=7)

    # --- (c) XYZ random access ---
    ax = axes[2]
    style.panel_label(ax, "(c)")

    axes_names = ["x", "y", "z"]
    x_pos = np.arange(3)
    width = 0.25

    for j, (cfg, fmt, color, alpha) in enumerate([
        ("A0_auto_final", "LZ4", style.COLORS["lz4"], 1.0),
        ("A1_force_rzfp_final", "RZFP", style.COLORS["rzfp"], 1.0),
    ]):
        means = []
        stds = []
        for ax_name in axes_names:
            row = agg[(agg["dataset"] == "20GB") & (agg["configuration"] == cfg) &
                     (agg["format"] == fmt) & (agg["device"] == "SSD") &
                     (agg["axis"] == ax_name) & (agg["pattern"] == "random")]
            if not row.empty:
                means.append(row.iloc[0]["mean_ms"] / 1000.0)
                stds.append(row.iloc[0]["std_ms"] / 1000.0)
            else:
                means.append(0)
                stds.append(0)

        offset = (j - 0.5) * width
        label = "LZ4" if fmt == "LZ4" else "RZFP"
        ax.errorbar(x_pos + offset, means, yerr=stds, color=color,
                   fmt=style.MARKERS[label.lower()], markersize=5, capsize=3,
                   linewidth=1.0, markeredgecolor="#333333", markeredgewidth=0.5,
                   label=label, alpha=alpha)
        ax.plot(x_pos + offset, means, color=color, linewidth=0.8, alpha=0.5)

    ax.set_xticks(x_pos)
    ax.set_xticklabels(["X", "Y", "Z"], fontsize=6)
    ax.set_ylabel("Random access time (s)", fontsize=7)
    ax.legend(fontsize=5.5)

    fig.subplots_adjust(wspace=0.35)
    style.savefig(fig, "Fig4_codec_ablation", outdir)
    plt.close(fig)

if __name__ == "__main__":
    make_figure()
