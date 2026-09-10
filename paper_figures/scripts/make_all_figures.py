#!/usr/bin/env python3
"""Generate all ERWT3D paper figures."""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))

def main():
    outdir = os.path.join(os.path.dirname(__file__), "..", "output")
    os.makedirs(outdir, exist_ok=True)

    print("=" * 50)
    print("ERWT3D Paper Figures — Generating All")
    print("=" * 50)

    from fig1_framework import make_figure as fig1
    print("\n[1/6] Fig1: ERWT3D framework...")
    fig1(outdir)

    from fig2_storage import make_figure as fig2
    print("[2/6] Fig2: Storage selection...")
    fig2(outdir)

    from fig3_access import make_figure as fig3
    print("[3/6] Fig3: Multi-axis access...")
    fig3(outdir)

    from fig4_ablation import make_figure as fig4
    print("[4/6] Fig4: Codec ablation...")
    fig4(outdir)

    from fig5_f3 import make_figure as fig5
    print("[5/6] Fig5: F3 real data...")
    fig5(outdir)

    from fig6_accuracy import make_figure as fig6
    print("[6/6] Fig6: RZFP accuracy...")
    fig6(outdir)

    print("\n" + "=" * 50)
    print("All figures generated.")
    print(f"Output: {os.path.abspath(outdir)}")
    print("=" * 50)

if __name__ == "__main__":
    main()
