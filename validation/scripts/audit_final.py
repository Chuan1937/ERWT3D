#!/usr/bin/env python3
"""FINAL audit: 25 checks for paper readiness."""
import json, csv, sys
from pathlib import Path

ALGORITHM_COMMIT = "0fa38a2e6ddbf78e2d052bff03bde11f9cbc7ba3"
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
    print("FINAL AUDIT (25 checks)")
    print("=" * 60)

    all_jsons = load_all_jsons(input_root)
    benchmark_jsons = [d for d in all_jsons if "benchmark_schema_version" in d and d.get("status") == "SUCCESS"]

    # 1. No 50GB
    ds_path = summaries_root / "dataset_summary.csv"
    ds_rows = []
    if ds_path.exists():
        with open(ds_path) as f:
            ds_rows = list(csv.DictReader(f))
    has_50gb = any(r["dataset"] == "50GB" for r in ds_rows)
    if check(not has_50gb, "No 50GB in dataset_summary"):
        passed += 1
    else:
        failed += 1

    # 2. No efwi3D
    summary_path = summaries_root / "EXPERIMENT_SUMMARY_FINAL.md"
    summary_text = summary_path.read_text() if summary_path.exists() else ""
    has_efwi = "efwi3D" in summary_text or "geophysical fidelity" in summary_text.lower()
    if check(not has_efwi, "No efwi3D in FINAL summary"):
        passed += 1
    else:
        failed += 1

    # 3. 20GB Auto = LZ4+XYZ
    auto_20gb = [d for d in benchmark_jsons if d.get("dataset") == "20GB" and d.get("configuration") == "A0_auto_final"]
    formats = set(d.get("format") for d in auto_20gb)
    layouts = set(d.get("layout") for d in auto_20gb)
    if check(formats == {"LZ4"} and layouts == {"LZ4+XYZ"}, f"20GB Auto = LZ4+XYZ"):
        passed += 1
    else:
        failed += 1

    # 4. F3 amplitude = LZ4+XYZ
    f3_amp = [d for d in benchmark_jsons if d.get("dataset") == "f3_amplitude"]
    f3_amp_fmt = set(d.get("format") for d in f3_amp)
    f3_amp_lay = set(d.get("layout") for d in f3_amp)
    if check(f3_amp_fmt == {"LZ4"} and f3_amp_lay == {"LZ4+XYZ"}, f"F3 amplitude = LZ4+XYZ"):
        passed += 1
    else:
        failed += 1

    # 5. F3 similarity = RZFP+XYZ
    f3_sim = [d for d in benchmark_jsons if d.get("dataset") == "f3_similarity"]
    f3_sim_fmt = set(d.get("format") for d in f3_sim)
    f3_sim_lay = set(d.get("layout") for d in f3_sim)
    if check(f3_sim_fmt == {"RZFP"} and f3_sim_lay == {"RZFP+XYZ"}, f"F3 similarity = RZFP+XYZ"):
        passed += 1
    else:
        failed += 1

    # 6. F3 similarity access format == RZFP
    if check(f3_sim_fmt == {"RZFP"}, f"F3 similarity access format = RZFP"):
        passed += 1
    else:
        failed += 1

    # 7. F3 similarity access all 6 groups n=5
    f3_sim_issues = []
    for ax in ["x", "y", "z"]:
        for pat in ["random", "continuous"]:
            matching = [d for d in f3_sim if d.get("axis") == ax and d.get("pattern") == pat]
            if len(matching) != EXPECTED_N:
                f3_sim_issues.append(f"f3_similarity/{ax}/{pat}: n={len(matching)}")
    if check(len(f3_sim_issues) == 0, f"F3 similarity 6 groups n={EXPECTED_N}"):
        passed += 1
    else:
        failed += 1
        for i in f3_sim_issues:
            print(f"    {i}")

    # 8. 20GB LZ4 bitwise_equal=True
    acc_path = input_root / "accuracy"
    acc_data = {}
    if acc_path.exists():
        for af in acc_path.glob("*.json"):
            try:
                d = json.loads(af.read_text())
                acc_data[(d.get("dataset"), d.get("format"))] = d
            except:
                pass
    lz4_20gb = acc_data.get(("20GB", "LZ4"), {})
    if check(lz4_20gb.get("bitwise_equal") is True, "20GB LZ4 bitwise_equal=True"):
        passed += 1
    else:
        failed += 1

    # 9. 20GB RZFP max_rel < 1e-3
    rzfp_20gb = acc_data.get(("20GB", "RZFP"), {})
    max_rel = float(rzfp_20gb.get("max_relative_error", 999))
    if check(max_rel < 0.001, f"20GB RZFP max_rel={max_rel:.6f} < 1e-3"):
        passed += 1
    else:
        failed += 1

    # 10. 20GB RZFP violations=0
    violations = int(rzfp_20gb.get("violation_count", -1))
    if check(violations == 0, f"20GB RZFP violations={violations}"):
        passed += 1
    else:
        failed += 1

    # 11. F3 similarity max_rel < 1e-3
    f3_sim_acc = acc_data.get(("f3_similarity", "RZFP"), {})
    f3_max_rel = float(f3_sim_acc.get("max_relative_error", 999))
    if check(f3_max_rel < 0.001, f"F3 similarity max_rel={f3_max_rel:.6f} < 1e-3"):
        passed += 1
    else:
        failed += 1

    # 12. F3 similarity violations=0
    f3_violations = int(f3_sim_acc.get("violation_count", -1))
    if check(f3_violations == 0, f"F3 similarity violations={f3_violations}"):
        passed += 1
    else:
        failed += 1

    # 13. RZFP accuracy RMSE from stream_accuracy/reader (not upper bound)
    rzfp_method = rzfp_20gb.get("verification_method", "")
    if check("upper" not in rzfp_method.lower() and rzfp_method != "", f"20GB RZFP method={rzfp_method} (not upper bound)"):
        passed += 1
    else:
        failed += 1

    # 14. RZFP accuracy NRMSE from real measurement
    rzfp_nrmse = rzfp_20gb.get("nrmse", 0)
    if check(float(rzfp_nrmse) > 0, f"20GB RZFP NRMSE={rzfp_nrmse} (measured)"):
        passed += 1
    else:
        failed += 1

    # 15. algorithm_commit = 0fa38a2...
    algo_issues = []
    for d in benchmark_jsons:
        ac = d.get("algorithm_commit", "")
        if not ac.startswith("0fa38a2"):
            algo_issues.append(f"{Path(d.get('_file','')).name}: algo={ac[:12]}")
    if check(len(algo_issues) == 0, f"algorithm_commit = 0fa38a2... ({len(algo_issues)} violations)"):
        passed += 1
    else:
        failed += 1

    # 16. validation_commit exists
    val_issues = []
    for d in benchmark_jsons:
        vc = d.get("validation_commit", "")
        if not vc or len(vc) < 7:
            val_issues.append(f"{Path(d.get('_file','')).name}: missing validation_commit")
    if check(len(val_issues) == 0, f"validation_commit exists ({len(val_issues)} violations)"):
        passed += 1
    else:
        failed += 1

    # 17. package_bytes > 0
    pkg_ok = all(int(r.get("final_package_bytes", 0)) > 0 for r in ds_rows) if ds_rows else False
    if check(pkg_ok, "package_bytes > 0"):
        passed += 1
    else:
        failed += 1

    # 18. physical_ratio > 0
    ratio_ok = all(float(r.get("physical_storage_ratio", 0)) > 0 for r in ds_rows) if ds_rows else False
    if check(ratio_ok, "physical_ratio > 0"):
        passed += 1
    else:
        failed += 1

    # 19. Raw X no old 0.2ms bug
    raw_x_bug = [d for d in benchmark_jsons
                 if d.get("configuration") == "raw_baseline" and d.get("axis") == "x"
                 and d.get("total_time_ms", 0) < 100]
    if check(len(raw_x_bug) == 0, f"Raw X no old 0.2ms bug ({len(raw_x_bug)} found)"):
        passed += 1
    else:
        failed += 1

    # 20. FINAL summary no 531x
    has_531 = "531x" in summary_text or "531×" in summary_text
    if check(not has_531, "FINAL summary no 531x speedup"):
        passed += 1
    else:
        failed += 1

    # 21. FINAL summary no 1003x
    has_1003 = "1003x" in summary_text or "1003×" in summary_text
    if check(not has_1003, "FINAL summary no 1003x speedup"):
        passed += 1
    else:
        failed += 1

    # 22. Raw Z headline speedup excluded
    import re
    # Check that Z rows in Table C show "—" not numeric speedup
    z_speedup_match = re.search(r"\| z \| .*\| \d+\.\d+x \|", summary_text)
    if check(z_speedup_match is None, "Raw Z headline speedup excluded"):
        passed += 1
    else:
        failed += 1

    # 23. FINAL F3 table no "—" for f3_amplitude or f3_similarity
    f3_path = summaries_root / "access_f3_per_slice.csv"
    f3_rows = []
    if f3_path.exists():
        with open(f3_path) as f:
            f3_rows = list(csv.DictReader(f))
    f3_datasets = set(r.get("dataset", "") for r in f3_rows)
    if check("f3_amplitude" in f3_datasets and "f3_similarity" in f3_datasets,
             f"F3 table has both datasets: {f3_datasets}"):
        passed += 1
    else:
        failed += 1

    # 24. Old F3 similarity LZ4 not in FINAL
    old_f3_sim_lz4 = [d for d in benchmark_jsons
                      if d.get("dataset") == "f3_similarity" and d.get("format") == "LZ4"]
    if check(len(old_f3_sim_lz4) == 0, f"Old F3 similarity LZ4 not in FINAL ({len(old_f3_sim_lz4)} found)"):
        passed += 1
    else:
        failed += 1

    # 25. No old 380s LZ4 results
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
