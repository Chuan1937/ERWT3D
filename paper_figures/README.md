# ERWT3D Paper Figures

Publication-quality figures for the ERWT3D paper (Applied Geophysics).

## Dependencies

```bash
python -m pip install numpy pandas matplotlib SciencePlots
```

## Usage

Generate all figures:

```bash
python paper_figures/scripts/make_all_figures.py
```

Generate individual figures:

```bash
python paper_figures/scripts/fig1_framework.py
python paper_figures/scripts/fig2_storage.py
python paper_figures/scripts/fig3_access.py
python paper_figures/scripts/fig4_ablation.py
python paper_figures/scripts/fig5_f3.py
python paper_figures/scripts/fig6_accuracy.py
```

Run figure audit:

```bash
python paper_figures/scripts/audit_figures.py
```

## Output

```
paper_figures/output/
    pdf/    # vector PDF (editable text)
    svg/    # vector SVG (editable text)
    png/    # 600 DPI raster
```

## Style

All figures use Nature/Science style via `style.py`:

- **Colors**: Okabe-Ito colorblind-friendly palette
  - Raw = gray, LZ4 = blue, RZFP = orange
- **Font**: Arial (fallback: Helvetica, Liberation Sans, DejaVu Sans)
- **Size**: 183mm double-column width
- **Font size**: 6-7pt body, 8pt panel labels
- **Font embedding**: pdf.fonttype=42 for editable text

## Data Source

All data from `validation/summaries_final/` — no hard-coded benchmark results.
