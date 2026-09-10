"""Fig5: F3 real seismic data visualization and latency.

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

def make_figure(outdir="paper_figures/output"):
    f3 = pd.read_csv(os.path.join(DATA_DIR, "access_f3_per_slice.csv"))

    fig = plt.figure(figsize=(style.WIDTH_DOUBLE, 120/25.4))

    # Create grid: 2 rows, 3 cols (top: seismic images, bottom: latency)
    gs = fig.add_gridspec(2, 3, hspace=0.4, wspace=0.35)

    # --- (a) F3 amplitude representative slice ---
    ax_amp = fig.add_subplot(gs[0, 0])
    style.panel_label(ax_amp, "(a)")
    ax_amp.set_title("F3 amplitude", fontsize=7, fontweight="bold")

    try:
        amp_data = np.fromfile(F3_AMP_RAW, dtype=np.float32).reshape(201, 201, 51)
        # Pick middle Z slice
        z_mid = 25
        slice_data = amp_data[:, :, z_mid]
        p = np.percentile(np.abs(slice_data), 99)
        ax_amp.imshow(slice_data.T, cmap="RdBu_r", vmin=-p, vmax=p, aspect="auto",
                     origin="lower", interpolation="bilinear")
        ax_amp.set_xlabel("X", fontsize=6)
        ax_amp.set_ylabel("Y", fontsize=6)
        ax_amp.tick_params(labelsize=5)
    except Exception as e:
        ax_amp.text(0.5, 0.5, f"Data not available\n{e}", transform=ax_amp.transAxes,
                   ha="center", va="center", fontsize=6)

    # --- (b) F3 similarity representative slice ---
    ax_sim = fig.add_subplot(gs[0, 1])
    style.panel_label(ax_sim, "(b)")
    ax_sim.set_title("F3 similarity", fontsize=7, fontweight="bold")

    try:
        sim_data = np.fromfile(F3_SIM_RAW, dtype=np.float32).reshape(191, 146, 51)
        z_mid = 25
        slice_data = sim_data[:, :, z_mid]
        ax_sim.imshow(slice_data.T, cmap="cividis", aspect="auto",
                     origin="lower", interpolation="bilinear")
        ax_sim.set_xlabel("X", fontsize=6)
        ax_sim.set_ylabel("Y", fontsize=6)
        ax_sim.tick_params(labelsize=5)
    except Exception as e:
        ax_sim.text(0.5, 0.5, f"Data not available\n{e}", transform=ax_sim.transAxes,
                   ha="center", va="center", fontsize=6)

    # --- (c) Empty panel for future use or legend ---
    ax_empty = fig.add_subplot(gs[0, 2])
    ax_empty.axis("off")

    # --- (d) F3 amplitude latency ---
    ax_lat_amp = fig.add_subplot(gs[1, 0:2])
    style.panel_label(ax_lat_amp, "(c)")
    ax_lat_amp.set_title("Per-slice latency", fontsize=7, fontweight="bold")

    f3_amp = f3[f3["dataset"] == "f3_amplitude"]
    f3_sim = f3[f3["dataset"] == "f3_similarity"]

    # Collect slice_latencies_ms from per-slice CSV (mean_slice_latency_ms per run)
    # For violin/box plots, use the mean_slice_latency_ms from each run
    positions_amp = []
    data_amp = []
    positions_sim = []
    data_sim = []

    axis_labels = ["X\nrandom", "X\ncont", "Y\nrandom", "Y\ncont", "Z\nrandom", "Z\ncont"]
    x_pos = np.arange(6)

    for i, (ax_name, pat) in enumerate([("x","random"),("x","continuous"),
                                         ("y","random"),("y","continuous"),
                                         ("z","random"),("z","continuous")]):
        # Amplitude
        vals = f3_amp[(f3_amp["axis"] == ax_name) & (f3_amp["pattern"] == pat)]["mean_slice_latency_ms"].values
        if len(vals) > 0:
            positions_amp.append(i)
            data_amp.append(vals)

        # Similarity
        vals = f3_sim[(f3_sim["axis"] == ax_name) & (f3_sim["pattern"] == pat)]["mean_slice_latency_ms"].values
        if len(vals) > 0:
            positions_sim.append(i)
            data_sim.append(vals)

    # Plot as grouped box + scatter
    width = 0.3
    for i, (pos, data) in enumerate(zip(positions_amp, data_amp)):
        bp = ax_lat_amp.boxplot(data, positions=[pos - width/2], widths=0.2,
                               patch_artist=True, showfliers=False)
        for patch in bp["boxes"]:
            patch.set_facecolor(style.COLORS["lz4"])
            patch.set_alpha(0.4)
        ax_lat_amp.scatter(np.full(len(data), pos - width/2), data,
                          color=style.COLORS["lz4"], s=10, alpha=0.7, zorder=3)

    for i, (pos, data) in enumerate(zip(positions_sim, data_sim)):
        bp = ax_lat_amp.boxplot(data, positions=[pos + width/2], widths=0.2,
                               patch_artist=True, showfliers=False)
        for patch in bp["boxes"]:
            patch.set_facecolor(style.COLORS["rzfp"])
            patch.set_alpha(0.4)
        ax_lat_amp.scatter(np.full(len(data), pos + width/2), data,
                          color=style.COLORS["rzfp"], s=10, alpha=0.7, zorder=3)

    ax_lat_amp.set_xticks(x_pos)
    ax_lat_amp.set_xticklabels(axis_labels, fontsize=5.5)
    ax_lat_amp.set_ylabel("Per-slice latency (ms)", fontsize=7)

    # Legend
    from matplotlib.patches import Patch
    ax_lat_amp.legend([Patch(facecolor=style.COLORS["lz4"], alpha=0.4),
                       Patch(facecolor=style.COLORS["rzfp"], alpha=0.4)],
                      ["F3 amplitude (LZ4)", "F3 similarity (RZFP)"],
                      fontsize=5.5, loc="upper right")

    # --- (e) F3 similarity latency (separate panel for clarity) ---
    ax_lat_sim = fig.add_subplot(gs[1, 2])
    style.panel_label(ax_lat_sim, "(d)")

    # Summary stats for annotation
    amp_means = f3_amp.groupby(["axis", "pattern"])["mean_slice_latency_ms"].mean()
    sim_means = f3_sim.groupby(["axis", "pattern"])["mean_slice_latency_ms"].mean()

    text = "F3 amplitude (LZ4)\n"
    for (ax_name, pat), val in amp_means.items():
        text += f"  {ax_name} {pat}: {val:.1f} ms\n"
    text += "\nF3 similarity (RZFP)\n"
    for (ax_name, pat), val in sim_means.items():
        text += f"  {ax_name} {pat}: {val:.1f} ms\n"

    ax_lat_sim.text(0.05, 0.95, text, transform=ax_lat_sim.transAxes,
                   fontsize=5.5, va="top", ha="left", family="monospace",
                   bbox=dict(boxstyle="round,pad=0.5", facecolor="#F8F8F8",
                            edgecolor="#CCCCCC", linewidth=0.5))
    ax_lat_sim.axis("off")

    style.savefig(fig, "Fig5_F3_real_data", outdir)
    plt.close(fig)

if __name__ == "__main__":
    make_figure()
