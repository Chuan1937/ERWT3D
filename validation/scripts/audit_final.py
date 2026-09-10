#!/usr/bin/env python3
"""FINAL audit: 20 checks for paper readiness."""
import json, csv, sys
from pathlib import Path

ALGORITHM_COMMIT = "0fa38a2"
EXPECTED_N = 5

def check(condition, msg):
    if not condition:
        print(f"  FAIL: {msg}")
        return False
    print(f"  PASS: {msg}")
    return True

def load_all_jsons(root):
    results = []
    for jf in sorted(root.rglob("*.json")):
        try:
            d = json.loads(jf.read_text())
            d["_file"] = str(jf)
            results.append(d)
        except:
            pass
    return results

def audit(input_root, summaries_root):
    input_root = Path(input_root)
    summaries_root = Path(summaries_root)
    passed = 0
    failed = 0

    print("=" * 60)
    print("FINAL AUDIT (20 checks)")
    print("=" * 60)

    all_jsons = load_all_jsons(input_root)
    benchmark_jsons = [d for d in all_jsons if "benchmark_schema_version" in d and d.get("status") == "SUCCESS"]

    # 1. dataset_summary has no 50GB
    ds_path = summaries_root / "dataset_summary.csv"
    ds_rows = []
    if ds_path.exists():
        with open(ds_path) as f:
            ds_rows = list(csv.DictReader(f))
    has_50gb = any(r["dataset"] == "50GB" for r in ds_rows)
    if check(not has_50gb, "dataset_summary has no 50GB"):
        passed += 1
    else:
        failed += 1

    # 2. package_bytes > 0 for all datasets
    pkg_ok = all(int(r.get("final_package_bytes", 0)) > 0 for r in ds_rows) if ds_rows else False
    if check(pkg_ok, f"package_bytes > 0 for all datasets ({len(ds_rows)} rows)"):
        passed += 1
    else:
        failed += 1

    # 3. physical_ratio > 0 for all datasets
    ratio_ok = all(float(r.get("physical_storage_ratio", 0)) > 0 for r in ds_rows) if ds_rows else False
    if check(ratio_ok, "physical_ratio > 0 for all datasets"):
        passed += 1
    else:
        failed += 1

    # 4. 20GB Auto = LZ4+XYZ
    auto_20gb = [d for d in benchmark_jsons if d.get("dataset") == "20GB" and d.get("configuration") == "A0_auto_final"]
    formats = set(d.get("format") for d in auto_20gb)
    layouts = set(d.get("layout") for d in auto_20gb)
    if check(formats == {"LZ4"} and layouts == {"LZ4+XYZ"}, f"20GB Auto = LZ4+XYZ (got {formats}, {layouts})"):
        passed += 1
    else:
        failed += 1

    # 5. 20GB Auto LZ4 bitwise equal
    acc_path = input_root / "accuracy"
    acc_files = list(acc_path.glob("*.json")) if acc_path.exists() else []
    acc_data = {}
    for af in acc_files:
        try:
            d = json.loads(af.read_text())
            acc_data[(d.get("dataset"), d.get("format"))] = d
        except:
            pass
    lz4_20gb = acc_data.get(("20GB", "LZ4"), {})
    if check(lz4_20gb.get("bitwise_equal") is True, "20GB Auto LZ4 bitwise equal"):
        passed += 1
    else:
        failed += 1

    # 6. Forced RZFP max_rel < 1e-3
    rzfp_20gb = acc_data.get(("20GB", "RZFP"), {})
    max_rel = rzfp_20gb.get("max_relative_error", 999)
    if check(max_rel < 0.001, f"Forced RZFP max_rel={max_rel:.6f} < 1e-3"):
        passed += 1
    else:
        failed += 1

    # 7. Forced RZFP violations = 0
    violations = rzfp_20gb.get("violation_count", -1)
    if check(violations == 0, f"Forced RZFP violations={violations}"):
        passed += 1
    else:
        failed += 1

    # 8. F3 similarity RZFP max_rel < 1e-3
    f3_sim = acc_data.get(("f3_similarity", "RZFP"), {})
    f3_max_rel = f3_sim.get("max_relative_error", 999)
    if check(f3_max_rel < 0.001, f"F3 similarity RZFP max_rel={f3_max_rel:.6f} < 1e-3"):
        passed += 1
    else:
        failed += 1

    # 9. Raw baseline XYZ random n=5
    raw_issues = []
    for ax in ["x", "y", "z"]:
        matching = [d for d in benchmark_jsons
                    if d.get("dataset") == "20GB" and d.get("configuration") == "raw_baseline"
                    and d.get("axis") == ax and d.get("pattern") == "random"]
        if len(matching) != EXPECTED_N:
            raw_issues.append(f"raw/{ax}/random: n={len(matching)}")
    if check(len(raw_issues) == 0, f"Raw baseline XYZ random n={EXPECTED_N} ({len(raw_issues)} issues)"):
        passed += 1
    else:
        failed += 1
        for r in raw_issues:
            print(f"    {r}")

    # 10. Raw baseline XYZ continuous n=5
    raw_cont_issues = []
    for ax in ["x", "y", "z"]:
        matching = [d for d in benchmark_jsons
                    if d.get("dataset") == "20GB" and d.get("configuration") == "raw_baseline"
                    and d.get("axis") == ax and d.get("pattern") == "continuous"]
        if len(matching) != EXPECTED_N:
            raw_cont_issues.append(f"raw/{ax}/continuous: n={len(matching)}")
    if check(len(raw_cont_issues) == 0, f"Raw baseline XYZ continuous n={EXPECTED_N} ({len(raw_cont_issues)} issues)"):
        passed += 1
    else:
        failed += 1
        for r in raw_cont_issues:
            print(f"    {r}")

    # 11. Forced RZFP 6 groups all n=5
    rzfp_issues = []
    for ax in ["x", "y", "z"]:
        for pat in ["random", "continuous"]:
            matching = [d for d in benchmark_jsons
                        if d.get("dataset") == "20GB" and d.get("configuration") == "A1_force_rzfp_final"
                        and d.get("axis") == ax and d.get("pattern") == pat]
            if len(matching) != EXPECTED_N:
                rzfp_issues.append(f"rzfp/{ax}/{pat}: n={len(matching)}")
    if check(len(rzfp_issues) == 0, f"Forced RZFP 6 groups n={EXPECTED_N} ({len(rzfp_issues)} issues)"):
        passed += 1
    else:
        failed += 1
        for r in rzfp_issues:
            print(f"    {r}")

    # 12. Auto 6 groups all n=5 (SSD only for main benchmark)
    auto_issues = []
    for ax in ["x", "y", "z"]:
        for pat in ["random", "continuous"]:
            matching = [d for d in benchmark_jsons
                        if d.get("dataset") == "20GB" and d.get("configuration") == "A0_auto_final"
                        and d.get("device") == "SSD"
                        and d.get("axis") == ax and d.get("pattern") == pat]
            if len(matching) != EXPECTED_N:
                auto_issues.append(f"auto/{ax}/{pat}: n={len(matching)}")
    if check(len(auto_issues) == 0, f"Auto SSD 6 groups n={EXPECTED_N} ({len(auto_issues)} issues)"):
        passed += 1
    else:
        failed += 1
        for r in auto_issues:
            print(f"    {r}")

    # 13. Forced LZ4 6 groups all n=5
    flz4_issues = []
    for ax in ["x", "y", "z"]:
        for pat in ["random", "continuous"]:
            matching = [d for d in benchmark_jsons
                        if d.get("dataset") == "20GB" and d.get("configuration") == "A1_force_lz4_final"
                        and d.get("axis") == ax and d.get("pattern") == pat]
            if len(matching) != EXPECTED_N:
                flz4_issues.append(f"force_lz4/{ax}/{pat}: n={len(matching)}")
    if check(len(flz4_issues) == 0, f"Forced LZ4 6 groups n={EXPECTED_N} ({len(flz4_issues)} issues)"):
        passed += 1
    else:
        failed += 1
        for r in flz4_issues:
            print(f"    {r}")

    # 14. F3 per-slice not empty
    f3_path = summaries_root / "access_f3_per_slice.csv"
    f3_rows = []
    if f3_path.exists():
        with open(f3_path) as f:
            f3_rows = list(csv.DictReader(f))
    if check(len(f3_rows) > 0, f"F3 per-slice not empty ({len(f3_rows)} rows)"):
        passed += 1
    else:
        failed += 1

    # 15. FINAL summary has no 50GB
    summary_path = summaries_root / "EXPERIMENT_SUMMARY_FINAL.md"
    summary_text = summary_path.read_text() if summary_path.exists() else ""
    has_50gb_summary = "50GB" in summary_text
    if check(not has_50gb_summary, "FINAL summary has no 50GB"):
        passed += 1
    else:
        failed += 1

    # 16. FINAL summary has no efwi3D
    has_efwi = "efwi3D" in summary_text or "geophysical" in summary_text.lower()
    if check(not has_efwi, "FINAL summary has no efwi3D"):
        passed += 1
    else:
        failed += 1

    # 17. FINAL summary has no package=0
    has_pkg0 = "package_bytes = 0" in summary_text or "Package Size = 0" in summary_text
    if check(not has_pkg0, "FINAL summary has no package=0"):
        passed += 1
    else:
        failed += 1

    # 18. FINAL summary has no SR=0
    has_sr0 = "SR = 0" in summary_text or "ratio = 0" in summary_text
    if check(not has_sr0, "FINAL summary has no SR=0"):
        passed += 1
    else:
        failed += 1

    # 19. No old raw_results_v2 read
    old_v2 = [d for d in all_jsons if "raw_results_v2" in d.get("_file", "")]
    if check(len(old_v2) == 0, f"No raw_results_v2 records ({len(old_v2)} found)"):
        passed += 1
    else:
        failed += 1

    # 20. No old 380s LZ4 results
    old_lz4 = [d for d in benchmark_jsons
               if d.get("format") == "LZ4" and d.get("total_time_ms", 0) > 300000
               and d.get("axis") == "x" and d.get("pattern") == "random"]
    if check(len(old_lz4) == 0, f"No old 380s LZ4 results ({len(old_lz4)} found)"):
        passed += 1
    else:
        failed += 1

    print("=" * 60)
    total = passed + failed
    print(f"RESULT: {passed}/{total} passed, {failed} failed")
    if failed == 0:
        print("FINAL STATUS = READY FOR PAPER")
    else:
        print("FINAL STATUS = NOT READY")
    print("=" * 60)
    return failed == 0

if __name__ == "__main__":
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument("--input-root", default="/mnt/f/CUP/results/raw_results_final")
    p.add_argument("--summaries-root", default="validation/summaries_final")
    args = p.parse_args()
    ok = audit(args.input_root, args.summaries_root)
    sys.exit(0 if ok else 1)
