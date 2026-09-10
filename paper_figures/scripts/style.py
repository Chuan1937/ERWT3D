"""Unified Nature/Science style for ERWT3D paper figures."""
import matplotlib
import matplotlib.pyplot as plt
import numpy as np

# --- Font embedding (editable text in PDF/SVG) ---
matplotlib.rcParams["pdf.fonttype"] = 42
matplotlib.rcParams["ps.fonttype"] = 42
matplotlib.rcParams["svg.fonttype"] = "none"

# --- Font selection with fallback ---
_FONT_CANDIDATES = ["Arial", "Helvetica", "Liberation Sans", "DejaVu Sans"]
import matplotlib.font_manager as fm
_available = {f.name for f in fm.fontManager.ttflist}
FONT = next((n for n in _FONT_CANDIDATES if n in _available), "DejaVu Sans")

# --- Apply SciencePlots style ---
try:
    import scienceplots
    plt.style.use(["science", "nature", "no-latex"])
except Exception:
    pass

# --- Override with our exact specifications ---
matplotlib.rcParams.update({
    "font.family": "sans-serif",
    "font.sans-serif": _FONT_CANDIDATES,
    "font.size": 7,
    "axes.labelsize": 7,
    "axes.titlesize": 7,
    "xtick.labelsize": 6,
    "ytick.labelsize": 6,
    "legend.fontsize": 6,
    "axes.linewidth": 0.7,
    "lines.linewidth": 1.0,
    "lines.markersize": 3.5,
    "xtick.major.width": 0.6,
    "ytick.major.width": 0.6,
    "xtick.minor.width": 0.4,
    "ytick.minor.width": 0.4,
    "xtick.major.size": 3,
    "ytick.major.size": 3,
    "xtick.minor.size": 1.5,
    "ytick.minor.size": 1.5,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "figure.dpi": 150,
    "savefig.dpi": 600,
    "savefig.bbox": "tight",
    "savefig.pad_inches": 0.02,
    "legend.frameon": False,
    "legend.borderaxespad": 0,
})

# --- Semantic color palette (colorblind-friendly) ---
COLORS = {
    "raw":       "#7F7F7F",   # neutral gray
    "erwt3d":    "#0072B2",   # blue
    "lz4":       "#0072B2",   # blue (same as erwt3d)
    "rzfp":      "#D55E00",   # vermillion/orange
    "aux":       "#009E73",   # bluish green
    "accent":    "#CC79A7",   # reddish purple
    "bg":        "#F0F0F0",   # light gray for excluded
}

MARKERS = {
    "raw":   "s",    # square
    "erwt3d": "o",   # circle
    "lz4":   "o",    # circle
    "rzfp":  "D",    # diamond
    "aux":   "^",    # triangle
}

LINES = {
    "raw":   "--",
    "erwt3d": "-",
    "lz4":   "-",
    "rzfp":  "-.",
}

# --- Figure widths (mm -> inches) ---
WIDTH_DOUBLE = 183 / 25.4   # ~7.20 in
WIDTH_SINGLE = 89 / 25.4    # ~3.50 in
HEIGHT_MAX   = 170 / 25.4   # ~6.69 in

# --- Panel label style ---
PANEL_LABEL = dict(
    fontsize=8,
    fontweight="bold",
    fontstyle="normal",
    va="top",
    ha="left",
)

# --- Helper: add panel label ---
def panel_label(ax, label, x=-0.12, y=1.08):
    """Add (a), (b), etc. panel label."""
    ax.text(x, y, label, transform=ax.transAxes, **PANEL_LABEL)

# --- Helper: save all formats ---
def savefig(fig, name, outdir="paper_figures/output"):
    """Save figure as PDF, SVG, and PNG."""
    import os
    for fmt in ["pdf", "svg", "png"]:
        path = os.path.join(outdir, fmt, f"{name}.{fmt}")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        dpi = 600 if fmt == "png" else None
        fig.savefig(path, dpi=dpi, bbox_inches="tight", pad_inches=0.02)
    print(f"  Saved: {name}.pdf/svg/png")
