#!/usr/bin/env python3
"""Audit ERWT3D paper figures for correctness and consistency."""
import sys, os, csv, json
from pathlib import Path

PROJ = Path(__file__).resolve().parent.parent.parent
SUMMARIES = PROJ / "validation" / "summaries_final"
OUTPUT = PROJ / "paper_figures" / "output"

def check(cond, msg):
    if not cond:
        print(f"  FAIL: {msg}")
        return False
    print(f"  PASS: {msg}")
    return True

def main():
    passed = 0
    failed = 0
    total = 0

    print("=" * 60)
    print("FIGURE AUDIT")
    print("=" * 60)

    # 1. Source CSVs exist
    for name in ["dataset_summary.csv", "accuracy.csv", "benchmark_aggregate.csv",
                  "access_f3_per_slice.csv", "variability.csv"]:
        ok = (SUMMARIES / name).exists()
        total += 1; passed += 1 if ok else 0; failed += 0 if ok else 1
        check(ok, f"Source CSV exists: {name}")

    # 2. No 50GB in dataset_summary
    ds = list(csv.DictReader(open(SUMMARIES / "dataset_summary.csv")))
    ok = all(r["dataset"] != "50GB" for r in ds)
    total += 1; passed += 1 if ok else 0; failed += 0 if ok else 1
    check(ok, "No 50GB in dataset_summary")

    # 3. No efwi3D reference
    summary_md = (SUMMARIES / "EXPERIMENT_SUMMARY_FINAL.md").read_text()
    ok = "efwi3D" not in summary_md
    total += 1; passed += 1 if ok else 0; failed += 0 if ok else 1
    check(ok, "No efwi3D in summary")

    # 4. F3 similarity = RZFP
    f3_sim = [r for r in csv.DictReader(open(SUMMARIES / "access_f3_per_slice.csv"))
              if r["dataset"] == "f3_similarity"]
    ok = all(r["format"] == "RZFP" for r in f3_sim)
    total += 1; passed += 1 if ok else 0; failed += 0 if ok else 1
    check(ok, "F3 similarity format = RZFP")

    # 5. 20GB Auto = LZ4
    auto_20gb = [r for r in csv.DictReader(open(SUMMARIES / "benchmark_aggregate.csv"))
                 if r["dataset"] == "20GB" and r["configuration"] == "A0_auto_final"]
    ok = all(r["format"] == "LZ4" for r in auto_20gb)
    total += 1; passed += 1 if ok else 0; failed += 0 if ok else 1
    check(ok, "20GB Auto = LZ4")

    # 6. No Raw Z speedup in summary
    import re
    ok = not re.search(r"\| z \| .*\| \d+\.\d+x \|", summary_md)
    total += 1; passed += 1 if ok else 0; failed += 0 if ok else 1
    check(ok, "Raw Z no headline speedup")

    # 7. All n=5 benchmark groups
    agg = list(csv.DictReader(open(SUMMARIES / "benchmark_aggregate.csv")))
    incomplete = [r for r in agg if int(r.get("n", 0)) != 5 and r.get("device") == "SSD"]
    ok = len(incomplete) == 0
    total += 1; passed += 1 if ok else 0; failed += 0 if ok else 1
    check(ok, f"All SSD benchmark groups n=5 ({len(incomplete)} incomplete)")

    # 8. No old 380s LZ4
    ok = not any(r["format"] == "LZ4" and float(r.get("mean_ms", 0)) > 300000
                 for r in agg if r["axis"] == "x" and r["pattern"] == "random")
    total += 1; passed += 1 if ok else 0; failed += 0 if ok else 1
    check(ok, "No old 380s LZ4 results")

    # 9. Physical ratio from measured
    ok = all(float(r.get("physical_storage_ratio", 0)) > 0 for r in ds)
    total += 1; passed += 1 if ok else 0; failed += 0 if ok else 1
    check(ok, "Physical ratio > 0 (measured)")

    # 10. Accuracy from FINAL accuracy.csv
    acc = list(csv.DictReader(open(SUMMARIES / "accuracy.csv")))
    ok = len(acc) == 4
    total += 1; passed += 1 if ok else 0; failed += 0 if ok else 1
    check(ok, f"Accuracy records = 4 (got {len(acc)})")

    # 11. max_rel < 1e-3
    rzfp_acc = [r for r in acc if r["format"] == "RZFP"]
    ok = all(float(r["max_relative_error"]) < 0.001 for r in rzfp_acc)
    total += 1; passed += 1 if ok else 0; failed += 0 if ok else 1
    check(ok, "RZFP max_rel < 1e-3")

    # 12. violations = 0
    ok = all(int(r["violation_count"]) == 0 for r in rzfp_acc)
    total += 1; passed += 1 if ok else 0; failed += 0 if ok else 1
    check(ok, "RZFP violations = 0")

    # 13-15. Output files exist
    figures = ["Fig1_ERWT3D_framework", "Fig2_storage_selection", "Fig3_multiaxis_access",
               "Fig4_codec_ablation", "Fig5_F3_real_data", "Fig6_RZFP_accuracy"]
    for fig in figures:
        for ext in ["pdf", "svg", "png"]:
            path = OUTPUT / ext / f"{fig}.{ext}"
            ok = path.exists()
            total += 1; passed += 1 if ok else 0; failed += 0 if ok else 1
            check(ok, f"{fig}.{ext} exists")

    # 16. PDF fonttype config (check source)
    style_path = PROJ / "paper_figures" / "scripts" / "style.py"
    style_content = style_path.read_text()
    ok = 'pdf.fonttype"] = 42' in style_content or 'pdf.fonttype", 42)' in style_content
    total += 1; passed += 1 if ok else 0; failed += 0 if ok else 1
    check(ok, "PDF fonttype=42 configured")

    # 17. File sizes reasonable (>1KB)
    for fig in figures:
        png_path = OUTPUT / "png" / f"{fig}.png"
        if png_path.exists():
            ok = png_path.stat().st_size > 1000
            total += 1; passed += 1 if ok else 0; failed += 0 if ok else 1
            check(ok, f"{fig}.png size > 1KB")

    # 18. Panel labels present (check source scripts)
    scripts_dir = PROJ / "paper_figures" / "scripts"
    all_scripts = list(scripts_dir.glob("fig*.py"))
    ok = all("panel_label" in s.read_text() for s in all_scripts)
    total += 1; passed += 1 if ok else 0; failed += 0 if ok else 1
    check(ok, "Panel labels in all figure scripts")

    # 19. No hard-coded benchmark arrays
    for s in all_scripts:
        content = s.read_text()
        # Check for suspicious hard-coded arrays of benchmark results
        has_hardcoded = "= [21." in content or "= [16." in content or "= [0.0009" in content
        ok = not has_hardcoded
        total += 1; passed += 1 if ok else 0; failed += 0 if ok else 1
        if not ok:
            check(False, f"No hard-coded results in {s.name}")

    # 20. Scripts independently reproducible
    ok = all("if __name__" in s.read_text() for s in all_scripts)
    total += 1; passed += 1 if ok else 0; failed += 0 if ok else 1
    check(ok, "All scripts have __main__ entry")

    print("=" * 60)
    print(f"RESULT: {passed}/{total} passed, {failed} failed")
    if failed == 0:
        print("FIGURE AUDIT = PASS")
    else:
        print("FIGURE AUDIT = FAIL")
    print("=" * 60)
    return failed == 0

if __name__ == "__main__":
    ok = main()
    sys.exit(0 if ok else 1)
