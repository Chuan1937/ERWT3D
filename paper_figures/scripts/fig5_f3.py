"""Fig5: F3 real seismic data and per-slice latency.

(a) Representative F3 amplitude slice (diverging colormap)
(b) Representative F3 similarity slice (sequential colormap)
(c) F3 amplitude per-slice latency (box + scatter)
(d) F3 similarity per-slice latency (box + scatter)

Source: /mnt/f/CUP/erwt3d-paper/storage_baselines/f3_amplitude/baseline.raw
        /mnt/f/CUP/erwt3d-paper/storage_baselines/f3_similarity/baseline.raw
        validation/summaries_final/access_f3_per_slice.csv
"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
import style
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

DATA_DIR = os.path.join(os.path.dirname(__file__), "..", "..", "validation", "summaries_final")
F3_AMP_RAW = "/mnt/f/CUP/erwt3d-paper/storage_baselines/f3_amplitude/baseline.raw"
F3_SIM_RAW = "/mnt/f/CUP/erwt3d-paper/storage_baselines/f3_similarity/baseline.raw"

def plot_latency_boxscatter(ax, f3_data, dataset_label, color, format_label):
    """Plot per-slice latency as box + scatter."""
    axis_labels = ["X\nrand", "X\ncont", "Y\nrand", "Y\ncont", "Z\nrand", "Z\ncont"]
    x_pos = np.arange(6)

    data_list = []
    for i, (ax_name, pat) in enumerate([("x","random"),("x","continuous"),
                                         ("y","random"),("y","continuous"),
                                         ("z","random"),("z","continuous")]):
        vals = f3_data[(f3_data["axis"] == ax_name) & (f3_data["pattern"] == pat)]["mean_slice_latency_ms"].values
        data_list.append(vals)

    positions = x_pos
    bp = ax.boxplot(data_list, positions=positions, widths=0.4,
                   patch_artist=True, showfliers=False,
                   boxprops=dict(linewidth=0.5),
                   whiskerprops=dict(linewidth=0.5),
                   capprops=dict(linewidth=0.5),
                   medianprops=dict(color="#333333", linewidth=0.8))

    for patch in bp["boxes"]:
        patch.set_facecolor(color)
        patch.set_alpha(0.3)

    # Scatter individual points
    for i, vals in enumerate(data_list):
        if len(vals) > 0:
            jitter = np.random.default_rng(42).uniform(-0.08, 0.08, len(vals))
            ax.scatter(np.full(len(vals), i) + jitter, vals,
                      color=color, s=8, alpha=0.7, zorder=3, edgecolors="none")

    ax.set_xticks(x_pos)
    ax.set_xticklabels(axis_labels, fontsize=5)
    ax.set_ylabel("Per-slice latency (ms)", fontsize=7)
    ax.set_title(f"{dataset_label} ({format_label})", fontsize=7, fontweight="bold")

def make_figure(outdir="paper_figures/output"):
    f3 = pd.read_csv(os.path.join(DATA_DIR, "access_f3_per_slice.csv"))
    f3_amp = f3[f3["dataset"] == "f3_amplitude"]
    f3_sim = f3[f3["dataset"] == "f3_similarity"]

    fig = plt.figure(figsize=(style.WIDTH_DOUBLE, 110/25.4))
    gs = fig.add_gridspec(2, 2, hspace=0.45, wspace=0.35)

    # --- (a) F3 amplitude slice ---
    ax_a = fig.add_subplot(gs[0, 0])
    style.panel_label(ax_a, "(a)")

    try:
        amp_data = np.fromfile(F3_AMP_RAW, dtype=np.float32).reshape(201, 201, 51)
        z_mid = 25
        sl = amp_data[:, :, z_mid]
        p = np.percentile(np.abs(sl), 99)
        im = ax_a.imshow(sl.T, cmap="RdBu_r", vmin=-p, vmax=p, aspect="auto",
                        origin="lower", interpolation="bilinear")
        ax_a.set_xlabel("X", fontsize=6)
        ax_a.set_ylabel("Y", fontsize=6)
        ax_a.tick_params(labelsize=5)
        ax_a.set_title("F3 amplitude", fontsize=7, fontweight="bold")
        plt.colorbar(im, ax=ax_a, shrink=0.8, pad=0.02)
    except Exception as e:
        ax_a.text(0.5, 0.5, f"Data not available", transform=ax_a.transAxes,
                 ha="center", va="center", fontsize=6)
        ax_a.set_title("F3 amplitude", fontsize=7, fontweight="bold")

    # --- (b) F3 similarity slice ---
    ax_b = fig.add_subplot(gs[0, 1])
    style.panel_label(ax_b, "(b)")

    try:
        sim_data = np.fromfile(F3_SIM_RAW, dtype=np.float32).reshape(191, 146, 51)
        z_mid = 25
        sl = sim_data[:, :, z_mid]
        im = ax_b.imshow(sl.T, cmap="cividis", aspect="auto",
                        origin="lower", interpolation="bilinear")
        ax_b.set_xlabel("X", fontsize=6)
        ax_b.set_ylabel("Y", fontsize=6)
        ax_b.tick_params(labelsize=5)
        ax_b.set_title("F3 similarity", fontsize=7, fontweight="bold")
        plt.colorbar(im, ax=ax_b, shrink=0.8, pad=0.02)
    except Exception as e:
        ax_b.text(0.5, 0.5, f"Data not available", transform=ax_b.transAxes,
                 ha="center", va="center", fontsize=6)
        ax_b.set_title("F3 similarity", fontsize=7, fontweight="bold")

    # --- (c) F3 amplitude latency ---
    ax_c = fig.add_subplot(gs[1, 0])
    style.panel_label(ax_c, "(c)")
    plot_latency_boxscatter(ax_c, f3_amp, "F3 amplitude", style.COLORS["lz4"], "LZ4")

    # --- (d) F3 similarity latency ---
    ax_d = fig.add_subplot(gs[1, 1])
    style.panel_label(ax_d, "(d)")
    plot_latency_boxscatter(ax_d, f3_sim, "F3 similarity", style.COLORS["rzfp"], "RZFP")

    style.savefig(fig, "Fig5_F3_real_data", outdir)
    plt.close(fig)

if __name__ == "__main__":
    make_figure()
