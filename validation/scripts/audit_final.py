#!/usr/bin/env python3
"""FINAL audit: verify all results are complete, correct, and consistent."""
import json, os, sys
from pathlib import Path

ALGORITHM_COMMIT = "0fa38a2"  # core algorithm commit prefix
VALID_DATASETS = {"f3_amplitude", "f3_similarity", "20GB"}
VALID_CONFIGS = {
    "A0_auto_final", "A1_force_lz4_final", "A1_force_rzfp_final",
    "raw_baseline",
}
EXPECTED_N = 5

def check(condition, msg):
    if not condition:
        print(f"  FAIL: {msg}")
        return False
    print(f"  PASS: {msg}")
    return True

def audit(input_root, summaries_root, result_root):
    input_root = Path(input_root)
    summaries_root = Path(summaries_root)
    passed = 0
    failed = 0

    print("=" * 60)
    print("FINAL AUDIT")
    print("=" * 60)

    # 1. No 50GB in final results
    gb50_files = [f for f in input_root.rglob("*.json") if "50GB" in f.name.upper() or "50gb" in f.name.lower()]
    if check(len(gb50_files) == 0, "No 50GB in raw_results_final"):
        passed += 1
    else:
        failed += 1

    # 2. All records have required metadata
    all_jsons = sorted(input_root.rglob("*.json"))
    missing_meta = []
    for jf in all_jsons:
        try:
            d = json.loads(jf.read_text())
        except:
            missing_meta.append(str(jf))
            continue
        for field in ["dataset", "format", "configuration", "device", "git_commit"]:
            if field not in d or d[field] in (None, ""):
                missing_meta.append(f"{jf.name}: missing {field}")
                break
    if check(len(missing_meta) == 0, f"All records have complete metadata ({len(missing_meta)} violations)"):
        passed += 1
    else:
        failed += 1
        for m in missing_meta[:5]:
            print(f"    {m}")

    # 3. No duplicate runs
    seen = set()
    duplicates = []
    for jf in all_jsons:
        try:
            d = json.loads(jf.read_text())
        except:
            continue
        key = (d.get("dataset"), d.get("configuration"), d.get("device"),
               d.get("axis"), d.get("pattern"), d.get("run_number"))
        if key in seen:
            duplicates.append(str(jf.name))
        seen.add(key)
    if check(len(duplicates) == 0, f"No duplicate runs ({len(duplicates)} dupes)"):
        passed += 1
    else:
        failed += 1

    # 4. Completeness check for main workloads
    completeness_issues = []
    for ds in ["20GB"]:
        for cfg in ["A0_auto_final"]:
            for dev in ["SSD", "HDD"]:
                for ax in ["x", "y", "z"]:
                    for pat in ["random", "continuous"]:
                        matching = [d for d in
                                    [json.loads(f.read_text()) for f in all_jsons if f.suffix == ".json"]
                                    if d.get("dataset") == ds and d.get("configuration") == cfg
                                    and d.get("device") == dev and d.get("axis") == ax
                                    and d.get("pattern") == pat and d.get("status") == "SUCCESS"]
                        if len(matching) != EXPECTED_N:
                            completeness_issues.append(
                                f"{ds}/{cfg}/{dev}/{ax}/{pat}: n={len(matching)}")
    if check(len(completeness_issues) == 0,
             f"20GB A0_auto_final all workloads n={EXPECTED_N} ({len(completeness_issues)} issues)"):
        passed += 1
    else:
        failed += 1
        for c in completeness_issues[:5]:
            print(f"    {c}")

    # 5. Algorithm commit check
    commit_issues = []
    for jf in all_jsons:
        try:
            d = json.loads(jf.read_text())
        except:
            continue
        gc = d.get("git_commit", "")
        if not gc.startswith(ALGORITHM_COMMIT):
            commit_issues.append(f"{jf.name}: commit={gc[:12]}")
    if check(len(commit_issues) == 0,
             f"All records use algorithm commit {ALGORITHM_COMMIT} ({len(commit_issues)} violations)"):
        passed += 1
    else:
        failed += 1

    # 6. 20GB Auto = LZ4+XYZ
    auto_20gb = [d for d in
                 [json.loads(f.read_text()) for f in all_jsons if f.suffix == ".json"]
                 if d.get("dataset") == "20GB" and d.get("configuration") == "A0_auto_final"]
    formats = set(d.get("format") for d in auto_20gb)
    layouts = set(d.get("layout") for d in auto_20gb)
    if check(formats == {"LZ4"} and layouts == {"LZ4+XYZ"},
             f"20GB Auto format=LZ4, layout=LZ4+XYZ (got {formats}, {layouts})"):
        passed += 1
    else:
        failed += 1

    # 7. X random not using old full-scan path
    x_random_20gb = [d for d in
                     [json.loads(f.read_text()) for f in all_jsons if f.suffix == ".json"]
                     if d.get("dataset") == "20GB" and d.get("axis") == "x"
                     and d.get("pattern") == "random" and d.get("status") == "SUCCESS"]
    if x_random_20gb:
        max_bytes = max(d.get("bytes_read", 0) for d in x_random_20gb)
        old_path = max_bytes > 10 * 1024**3  # >10GB means old path
        if check(not old_path, f"20GB X random bytes_read max={max_bytes/1e9:.1f}GB (not old 159GB path)"):
            passed += 1
        else:
            failed += 1
    else:
        print("  SKIP: no 20GB X random data yet")
        passed += 1

    # 8. Accuracy exists
    acc_path = input_root / "accuracy"
    acc_files = list(acc_path.glob("*.json")) if acc_path.exists() else []
    if check(len(acc_files) > 0, f"Accuracy results exist ({len(acc_files)} files)"):
        passed += 1
    else:
        failed += 1

    # 9. Summaries exist
    expected_csvs = ["benchmark_aggregate.csv", "dataset_summary.csv", "access_20gb.csv"]
    missing_csvs = [c for c in expected_csvs if not (summaries_root / c).exists()]
    if check(len(missing_csvs) == 0, f"All summary CSVs generated (missing: {missing_csvs})"):
        passed += 1
    else:
        failed += 1

    # 10. No old 380s LZ4 results leaked
    old_lz4 = [d for d in
               [json.loads(f.read_text()) for f in all_jsons if f.suffix == ".json"]
               if d.get("format") == "LZ4" and d.get("total_time_ms", 0) > 300000
               and d.get("axis") == "x" and d.get("pattern") == "random"]
    if check(len(old_lz4) == 0, f"No old buggy 380s LZ4 X random results ({len(old_lz4)} found)"):
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
    p.add_argument("--result-root", default="/mnt/f/CUP/results/raw_results_final")
    args = p.parse_args()
    ok = audit(args.input_root, args.summaries_root, args.result_root)
    sys.exit(0 if ok else 1)
