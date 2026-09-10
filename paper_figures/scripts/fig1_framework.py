"""Fig1: ERWT3D method framework — 5-panel flowchart.

(a) Data input: characteristics, access demand, storage budget
(b) Adaptive representation construction: LZ4 vs RZFP paths with planner selection
(c) Single-file organization
(d) Unified multi-axis access: reader pipeline
(e) Slice output: YZ/XZ/XY

Source: conceptual — no data CSV required.
"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
import style
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch
import numpy as np

def box(ax, x, y, w, h, text, fc="#FFFFFF", ec="#333333", tc="#333333",
        fs=6, lw=0.7, bold=False, rounded=True, zorder=2):
    """Draw a rounded box with centered text."""
    style_kw = "round,pad=0.015" if rounded else "square,pad=0"
    b = FancyBboxPatch((x, y), w, h, boxstyle=style_kw,
                       facecolor=fc, edgecolor=ec, linewidth=lw, zorder=zorder)
    ax.add_patch(b)
    weight = "bold" if bold else "normal"
    ax.text(x + w/2, y + h/2, text, ha="center", va="center",
            fontsize=fs, color=tc, fontweight=weight, zorder=zorder+1)
    return b

def arrow(ax, x1, y1, x2, y2, color="#555555", lw=0.6):
    """Draw a simple arrow."""
    ax.annotate("", xy=(x2, y2), xytext=(x1, y1),
                arrowprops=dict(arrowstyle="-|>", color=color, lw=lw,
                               mutation_scale=6), zorder=1)

def vline(ax, x, y1, y2, color="#555555", lw=0.4, ls="-"):
    ax.plot([x, x], [y1, y2], color=color, lw=lw, ls=ls, zorder=0)

def hline(ax, x1, x2, y, color="#555555", lw=0.4, ls="-"):
    ax.plot([x1, x2], [y, y], color=color, lw=lw, ls=ls, zorder=0)

def make_figure(outdir="paper_figures/output"):
    fig, axes = plt.subplots(1, 5, figsize=(style.WIDTH_DOUBLE, 80/25.4))
    for ax in axes:
        ax.set_xlim(0, 1)
        ax.set_ylim(0, 1)
        ax.axis("off")

    # ====================================================================
    # (a) Data input
    # ====================================================================
    ax = axes[0]
    style.panel_label(ax, "(a)")
    ax.text(0.5, 0.96, "Data input", ha="center", va="top", fontsize=7, fontweight="bold")

    # 3D volume icon (simple block)
    from mpl_toolkits.mplot3d.art3d import Poly3DCollection
    # Instead, draw a stylized cube with lines
    cx, cy = 0.35, 0.62
    s = 0.22
    # Front face
    ax.plot([cx, cx+s, cx+s, cx, cx], [cy, cy, cy+s, cy+s, cy], color="#0072B2", lw=0.8, zorder=2)
    # Back face offset
    dx, dy = 0.10, 0.08
    ax.plot([cx+dx, cx+s+dx, cx+s+dx, cx+dx, cx+dx],
            [cy+dy, cy+dy, cy+s+dy, cy+s+dy, cy+dy], color="#0072B2", lw=0.5, alpha=0.5, zorder=1)
    # Connecting edges
    for px, py in [(cx, cy), (cx+s, cy), (cx+s, cy+s), (cx, cy+s)]:
        ax.plot([px, px+dx], [py, py+dy], color="#0072B2", lw=0.5, alpha=0.5, zorder=1)
    ax.text(cx + s/2, cy + s/2, "3-D\nfloat32\nvolume", ha="center", va="center",
            fontsize=5.5, color="#0072B2", fontweight="bold", zorder=3)

    # Input characteristics
    items = [
        ("Data characteristics", "#555555"),
        ("Access demand", "#555555"),
        ("Storage budget", "#555555"),
    ]
    y_start = 0.42
    for i, (label, color) in enumerate(items):
        y = y_start - i * 0.13
        box(ax, 0.05, y, 0.90, 0.10, label, fc="#F5F5F5", ec=color, tc=color, fs=5.5)

    # Arrow from cube to items
    arrow(ax, 0.5, 0.62, 0.5, 0.42 + 0.10)

    # ====================================================================
    # (b) Adaptive representation construction
    # ====================================================================
    ax = axes[1]
    style.panel_label(ax, "(b)")
    ax.text(0.5, 0.96, "Representation", ha="center", va="top", fontsize=7, fontweight="bold")

    # Planner box at top
    box(ax, 0.08, 0.80, 0.84, 0.10, "Planner\n(compression · access · storage)",
        fc="#009E73", ec="#009E73", tc="white", fs=5.5, bold=True)

    # Selection arrow
    arrow(ax, 0.30, 0.80, 0.30, 0.70)
    arrow(ax, 0.70, 0.80, 0.70, 0.70)

    # Selection box
    box(ax, 0.15, 0.61, 0.70, 0.08, "Format selection",
        fc="#F0F0F0", ec="#999999", tc="#555555", fs=5.5, bold=True)

    # Two paths
    arrow(ax, 0.30, 0.61, 0.30, 0.52)
    arrow(ax, 0.70, 0.61, 0.70, 0.52)

    # LZ4 path (left)
    box(ax, 0.02, 0.32, 0.46, 0.20,
        "LZ4 path (lossless)\n\n64³ superblock\nLZ4 / raw fallback\nX/Y/Z axis-plane sections",
        fc="#D6EAF8", ec=style.COLORS["lz4"], tc="#1A5276", fs=5.0)

    # RZFP path (right)
    box(ax, 0.52, 0.32, 0.46, 0.20,
        "RZFP path (error-bounded)\n\n4³ leaf\nStrict error verification\nRZFP / raw fallback\nX/Y/Z axis-leaf sections",
        fc="#FDEBD0", ec=style.COLORS["rzfp"], tc="#7B3F00", fs=5.0)

    # Merge arrow
    arrow(ax, 0.25, 0.32, 0.50, 0.22)
    arrow(ax, 0.75, 0.32, 0.50, 0.22)

    # Output
    box(ax, 0.15, 0.10, 0.70, 0.12, "Single .erwt3d package\n(section directory)",
        fc="#333333", ec="#333333", tc="white", fs=5.5, bold=True)

    # ====================================================================
    # (c) Single-file organization
    # ====================================================================
    ax = axes[2]
    style.panel_label(ax, "(c)")
    ax.text(0.5, 0.96, "Package layout", ha="center", va="top", fontsize=7, fontweight="bold")

    sections = [
        ("Header", 0.82, 0.08, "#555555"),
        ("Main representation\n& index", 0.64, 0.18, style.COLORS["lz4"]),
        ("Embedded X sections", 0.50, 0.12, style.COLORS["erwt3d"]),
        ("Embedded Y sections", 0.38, 0.12, style.COLORS["aux"]),
        ("Embedded Z sections", 0.26, 0.12, style.COLORS["rzfp"]),
        ("Section directory", 0.12, 0.12, "#333333"),
    ]
    for i, (label, y, h, color) in enumerate(sections):
        box(ax, 0.08, y, 0.84, h, label, fc=color, ec="#333333", tc="white" if i != 0 else "white", fs=5.0, bold=(i==0))

    # Arrows between sections
    for i in range(len(sections) - 1):
        y_from = sections[i][1]
        y_to = sections[i+1][1] + sections[i+1][2]
        arrow(ax, 0.50, y_from, 0.50, y_to)

    # ====================================================================
    # (d) Unified multi-axis access
    # ====================================================================
    ax = axes[3]
    style.panel_label(ax, "(d)")
    ax.text(0.5, 0.96, "Unified reader", ha="center", va="top", fontsize=7, fontweight="bold")

    steps = [
        "Slice request\n(axis + index)",
        "Section lookup",
        "Ordered I/O",
        "Decode\n& reorder",
    ]
    y_positions = [0.78, 0.58, 0.38, 0.18]
    colors = ["#555555", style.COLORS["erwt3d"], style.COLORS["aux"], style.COLORS["rzfp"]]

    for i, (label, y, color) in enumerate(zip(steps, y_positions, colors)):
        box(ax, 0.10, y, 0.80, 0.15, label, fc=color, ec="#333333", tc="white", fs=5.5, bold=True)
        if i < len(steps) - 1:
            arrow(ax, 0.50, y, 0.50, y_positions[i+1] + 0.15)

    # ====================================================================
    # (e) Slice output
    # ====================================================================
    ax = axes[4]
    style.panel_label(ax, "(e)")
    ax.text(0.5, 0.96, "Slice output", ha="center", va="top", fontsize=7, fontweight="bold")

    slices = [
        ("YZ slice\n(X-fixed)", style.COLORS["erwt3d"]),
        ("XZ slice\n(Y-fixed)", style.COLORS["aux"]),
        ("XY slice\n(Z-fixed)", style.COLORS["rzfp"]),
    ]
    for i, (label, color) in enumerate(slices):
        y = 0.70 - i * 0.25
        box(ax, 0.10, y, 0.80, 0.18, label, fc=color, ec="#333333", tc="white", fs=5.5, bold=True)
        if i < len(slices) - 1:
            arrow(ax, 0.50, y, 0.50, y - 0.07)

    # "Standard slice output" at bottom
    box(ax, 0.15, 0.02, 0.70, 0.10, "Standard slice output",
        fc="#F0F0F0", ec="#555555", tc="#333333", fs=5.5, bold=True)
    arrow(ax, 0.50, 0.20, 0.50, 0.12)

    fig.subplots_adjust(wspace=0.05, left=0.01, right=0.99)
    style.savefig(fig, "Fig1_ERWT3D_framework", outdir)
    plt.close(fig)

if __name__ == "__main__":
    make_figure()
