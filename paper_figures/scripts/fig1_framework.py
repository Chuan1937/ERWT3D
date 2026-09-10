"""Fig1: ERWT3D method framework diagram.

Source: conceptual — no data CSV required.
"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
import style
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch
import numpy as np

def draw_box(ax, x, y, w, h, text, color="#0072B2", textcolor="white", fontsize=6, lw=0.7):
    box = FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.02",
                         facecolor=color, edgecolor="#333333", linewidth=lw)
    ax.add_patch(box)
    ax.text(x + w/2, y + h/2, text, ha="center", va="center",
            fontsize=fontsize, color=textcolor, fontweight="bold")
    return box

def draw_arrow(ax, x1, y1, x2, y2, color="#333333"):
    ax.annotate("", xy=(x2, y2), xytext=(x1, y1),
                arrowprops=dict(arrowstyle="-|>", color=color, lw=0.8))

def make_figure(outdir="paper_figures/output"):
    fig, axes = plt.subplots(1, 3, figsize=(style.WIDTH_DOUBLE, 85/25.4))
    for ax in axes:
        ax.set_xlim(0, 1)
        ax.set_ylim(0, 1)
        ax.axis("off")

    # --- (a) Adaptive conversion ---
    ax = axes[0]
    style.panel_label(ax, "(a)")
    ax.text(0.5, 0.97, "Adaptive conversion", ha="center", va="top",
            fontsize=7, fontweight="bold")

    # Input
    draw_box(ax, 0.15, 0.82, 0.7, 0.10, "Raw 3-D float32 volume", color="#666666", fontsize=6)
    draw_arrow(ax, 0.5, 0.82, 0.5, 0.74)

    # Probe
    draw_box(ax, 0.1, 0.64, 0.8, 0.10, "Data probe & planner", color="#009E73", fontsize=6)

    draw_arrow(ax, 0.3, 0.64, 0.3, 0.56)
    draw_arrow(ax, 0.7, 0.64, 0.7, 0.56)

    # LZ4 path
    draw_box(ax, 0.02, 0.42, 0.44, 0.14,
             "LZ4 path\nLossless · Axis-plane", color=style.COLORS["lz4"], fontsize=5.5)
    # RZFP path
    draw_box(ax, 0.54, 0.42, 0.44, 0.14,
             "RZFP path\nError-bounded · 4³ leaf", color=style.COLORS["rzfp"], fontsize=5.5)

    draw_arrow(ax, 0.24, 0.42, 0.5, 0.34)
    draw_arrow(ax, 0.76, 0.42, 0.5, 0.34)

    # Single package
    draw_box(ax, 0.15, 0.22, 0.7, 0.12, "Single .erwt3d package\nEmbedded section directory",
             color="#333333", fontsize=6)

    draw_arrow(ax, 0.5, 0.22, 0.5, 0.14)

    # Reader
    draw_box(ax, 0.1, 0.02, 0.8, 0.12, "Unified X / Y / Z slice reader",
             color=style.COLORS["erwt3d"], fontsize=6)

    # --- (b) Single-file organization ---
    ax = axes[1]
    style.panel_label(ax, "(b)")
    ax.text(0.5, 0.97, "Single-file layout", ha="center", va="top",
            fontsize=7, fontweight="bold")

    # File structure
    sections = [
        ("Header\n256 B", 0.85, 0.08, "#555555"),
        ("Data area\nSuperblocks", 0.72, 0.20, style.COLORS["lz4"]),
        ("Embedded X sections", 0.56, 0.16, style.COLORS["erwt3d"]),
        ("Embedded Y sections", 0.42, 0.16, style.COLORS["aux"]),
        ("Embedded Z sections", 0.28, 0.16, style.COLORS["rzfp"]),
        ("Section directory", 0.14, 0.12, "#333333"),
    ]
    for i, (label, y, h, color) in enumerate(sections):
        draw_box(ax, 0.1, y, 0.8, h, label, color=color, fontsize=5.5)
        if i < len(sections) - 1:
            ny, nh = sections[i+1][1], sections[i+1][2]
            draw_arrow(ax, 0.5, y, 0.5, ny + nh)

    # --- (c) Unified slice access ---
    ax = axes[2]
    style.panel_label(ax, "(c)")
    ax.text(0.5, 0.97, "Unified slice access", ha="center", va="top",
            fontsize=7, fontweight="bold")

    # Package
    draw_box(ax, 0.25, 0.78, 0.5, 0.12, ".erwt3d", color="#333333", fontsize=6)

    # Three arrows to X/Y/Z
    labels = [("X slice", 0.15, style.COLORS["erwt3d"]),
              ("Y slice", 0.50, style.COLORS["aux"]),
              ("Z slice", 0.85, style.COLORS["rzfp"])]
    for label, x, color in labels:
        draw_arrow(ax, 0.5, 0.78, x, 0.66)
        draw_box(ax, x-0.15, 0.52, 0.30, 0.14, label, color=color, fontsize=6)

    # Output
    for i, (label, x, color) in enumerate(labels):
        draw_arrow(ax, x, 0.52, x, 0.40)
        draw_box(ax, x-0.15, 0.26, 0.30, 0.14, f"{label}\noutput", color=color, fontsize=5)

    # Access benefit
    ax.text(0.5, 0.12, "Balanced multi-axis\naccess performance",
            ha="center", va="center", fontsize=6, fontstyle="italic",
            color="#555555")

    fig.subplots_adjust(wspace=0.08)
    style.savefig(fig, "Fig1_ERWT3D_framework", outdir)
    plt.close(fig)

if __name__ == "__main__":
    make_figure()
