#!/usr/bin/env python3
"""FINAL collector: strict validation, no 50GB, proper per-slice stats."""
import argparse, csv, json, math, os, statistics, sys
from collections import defaultdict
from pathlib import Path

REQUIRED_FIELDS = [
    "benchmark_schema_version", "dataset", "format", "layout",
    "configuration", "device", "algorithm_commit", "validation_commit", "axis", "pattern", "run_number",
]

ACCURACY_REQUIRED = ["dataset", "format"]

REJECTED_DATASETS = {"50GB"}

def percentile(values, q):
    if not values:
        return float("nan")
    s = sorted(values)
    k = (len(s) - 1) * q / 100.0
    f = math.floor(k)
    c = math.ceil(k)
    if f == c:
        return s[int(k)]
    return s[f] * (c - k) + s[c] * (k - f)

def is_accuracy_record(data, path):
    return "accuracy" in str(path).lower() or "max_relative_error" in data or "bitwise_equal" in data

def validate_record(data, path):
    errors = []
    ds = data.get("dataset", "")
    if ds in REJECTED_DATASETS:
        errors.append(f"rejected dataset: {ds}")
    if is_accuracy_record(data, path):
        for field in ACCURACY_REQUIRED:
            if field not in data or data[field] in (None, ""):
                errors.append(f"missing or empty {field}")
    else:
        for field in REQUIRED_FIELDS:
            if field not in data or data[field] in (None, "", 0):
                errors.append(f"missing or empty {field}")
        if data.get("status") != "SUCCESS":
            errors.append(f"status={data.get('status')}")
    return errors

def compute_per_slice(slice_latencies):
    if not slice_latencies:
        return {}
    return {
        "slice_count": len(slice_latencies),
        "mean_slice_latency_ms": statistics.mean(slice_latencies),
        "median_slice_latency_ms": percentile(slice_latencies, 50),
        "p95_slice_latency_ms": percentile(slice_latencies, 95),
        "p99_slice_latency_ms": percentile(slice_latencies, 99),
    }

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-root", required=True)
    parser.add_argument("--output-root", required=True)
    args = parser.parse_args()

    input_root = Path(args.input_root)
    output_root = Path(args.output_root)
    output_root.mkdir(parents=True, exist_ok=True)

    all_records = []
    accuracy_records = []
    errors_by_file = {}
    seen_keys = set()

    for json_file in sorted(input_root.rglob("*.json")):
        try:
            data = json.loads(json_file.read_text())
        except json.JSONDecodeError as e:
            errors_by_file[str(json_file)] = [f"JSON parse error: {e}"]
            continue

        errs = validate_record(data, json_file)
        if errs:
            errors_by_file[str(json_file)] = errs
            continue

        if is_accuracy_record(data, json_file):
            data["_source_file"] = str(json_file.relative_to(input_root))
            accuracy_records.append(data)
            continue

        # Duplicate detection for benchmark records
        key = (
            data["dataset"], data["configuration"], data["format"],
            data["layout"], data["device"], data["axis"], data["pattern"],
            data["run_number"],
        )
        if key in seen_keys:
            errors_by_file[str(json_file)] = [f"duplicate run: {key}"]
            continue
        seen_keys.add(key)

        latencies = data.get("slice_latencies_ms", [])
        per_slice = compute_per_slice(latencies)
        data.update(per_slice)
        data["_source_file"] = str(json_file.relative_to(input_root))
        all_records.append(data)

    if errors_by_file:
        err_path = output_root / "rejected_records.log"
        with open(err_path, "w") as f:
            for p, errs in sorted(errors_by_file.items()):
                for e in errs:
                    f.write(f"REJECT {p}: {e}\n")
        print(f"[WARN] {len(errors_by_file)} records rejected. See {err_path}")

    print(f"[OK] {len(all_records)} valid benchmark records, {len(accuracy_records)} accuracy records")

    # --- Group for aggregation ---
    def agg_key(r):
        return (r["dataset"], r["configuration"], r["format"], r["layout"],
                r["device"], r["axis"], r["pattern"])

    groups = defaultdict(list)
    for r in all_records:
        groups[agg_key(r)].append(r)

    # --- benchmark_aggregate.csv ---
    agg_rows = []
    for key in sorted(groups.keys()):
        recs = groups[key]
        ds, cfg, fmt, lay, dev, ax, pat = key
        times = [r.get("total_time_ms", 0) for r in recs]
        n = len(times)
        mean_t = statistics.mean(times) if times else 0
        std_t = statistics.stdev(times) if n > 1 else 0
        cv = (std_t / mean_t * 100) if mean_t else 0

        all_slices = []
        for r in recs:
            all_slices.extend(r.get("slice_latencies_ms", []))

        completeness = "COMPLETE" if n == 5 else f"INCOMPLETE(n={n})"
        stability = "STABLE" if cv <= 5 else "VARIABLE"

        agg_rows.append({
            "dataset": ds, "configuration": cfg, "format": fmt, "layout": lay,
            "device": dev, "axis": ax, "pattern": pat,
            "n": n, "completeness": completeness, "stability": stability,
            "mean_ms": round(mean_t, 1), "std_ms": round(std_t, 1),
            "cv_percent": round(cv, 1),
            "median_ms": round(percentile(times, 50), 1),
            "p95_ms": round(percentile(times, 95), 1),
            "p99_ms": round(percentile(times, 99), 1),
            "slice_count": len(all_slices),
            "mean_slice_latency_ms": round(statistics.mean(all_slices), 2) if all_slices else 0,
            "median_slice_latency_ms": round(percentile(all_slices, 50), 2) if all_slices else 0,
            "p95_slice_latency_ms": round(percentile(all_slices, 95), 2) if all_slices else 0,
            "p99_slice_latency_ms": round(percentile(all_slices, 99), 2) if all_slices else 0,
        })

    agg_path = output_root / "benchmark_aggregate.csv"
    if agg_rows:
        with open(agg_path, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(agg_rows[0].keys()))
            w.writeheader()
            w.writerows(agg_rows)
        print(f"[OK] {agg_path} ({len(agg_rows)} groups)")

    # --- access_20gb.csv ---
    access_rows = [r for r in all_records if r["dataset"] == "20GB" and r["configuration"] == "A0_auto_final"]
    if access_rows:
        p = output_root / "access_20gb.csv"
        fields = ["dataset","configuration","format","device","axis","pattern","run_number",
                  "total_time_ms","slice_count","bytes_read","pread_calls",
                  "mean_slice_latency_ms","median_slice_latency_ms","p95_slice_latency_ms","p99_slice_latency_ms"]
        with open(p, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
            w.writeheader()
            w.writerows(access_rows)
        print(f"[OK] {p} ({len(access_rows)} rows)")

    # --- access_f3_per_slice.csv ---
    # For f3_similarity, only accept RZFP records for FINAL
    f3_rows = []
    for r in all_records:
        if r["dataset"] == "f3_amplitude":
            f3_rows.append(r)
        elif r["dataset"] == "f3_similarity" and r.get("format") == "RZFP":
            f3_rows.append(r)
    if f3_rows:
        p = output_root / "access_f3_per_slice.csv"
        fields = ["dataset","configuration","format","layout","device","axis","pattern","run_number",
                  "total_time_ms","slice_count","mean_slice_latency_ms",
                  "median_slice_latency_ms","p95_slice_latency_ms","p99_slice_latency_ms"]
        with open(p, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
            w.writeheader()
            w.writerows(f3_rows)
        print(f"[OK] {p} ({len(f3_rows)} rows)")

    # --- ablation_codec_20gb.csv ---
    ablation_configs = {"A0_auto_final", "A1_force_lz4_final", "A1_force_rzfp_final"}
    abl_rows = [r for r in all_records if r["dataset"] == "20GB" and r["configuration"] in ablation_configs]
    if abl_rows:
        p = output_root / "ablation_codec_20gb.csv"
        fields = ["dataset","configuration","format","layout","device","axis","pattern","run_number",
                  "total_time_ms","slice_count","mean_slice_latency_ms"]
        with open(p, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
            w.writeheader()
            w.writerows(abl_rows)
        print(f"[OK] {p} ({len(abl_rows)} rows)")

    # --- baseline_raw_20gb.csv ---
    raw_rows = [r for r in all_records if r["dataset"] == "20GB" and r["configuration"] == "raw_baseline"]
    if raw_rows:
        p = output_root / "baseline_raw_20gb.csv"
        fields = ["dataset","configuration","device","axis","pattern","run_number",
                  "total_time_ms","slice_count","mean_slice_latency_ms"]
        with open(p, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
            w.writeheader()
            w.writerows(raw_rows)
        print(f"[OK] {p} ({len(raw_rows)} rows)")

    # --- variability.csv ---
    var_rows = []
    for key in sorted(groups.keys()):
        recs = groups[key]
        ds, cfg, fmt, lay, dev, ax, pat = key
        times = [r.get("total_time_ms", 0) for r in recs]
        n = len(times)
        mean_t = statistics.mean(times) if times else 0
        std_t = statistics.stdev(times) if n > 1 else 0
        cv = (std_t / mean_t * 100) if mean_t else 0
        # Raw Z is cache-sensitive
        reliability = "cache_sensitive" if (cfg == "raw_baseline" and ax == "z") else "normal"
        var_rows.append({
            "dataset": ds, "configuration": cfg, "format": fmt, "device": dev,
            "axis": ax, "pattern": pat,
            "mean": round(mean_t, 1), "std": round(std_t, 1),
            "cv_percent": round(cv, 1), "reliability": reliability,
        })
    if var_rows:
        p = output_root / "variability.csv"
        with open(p, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(var_rows[0].keys()))
            w.writeheader()
            w.writerows(var_rows)
        print(f"[OK] {p} ({len(var_rows)} rows)")

    # --- accuracy.csv ---
    if accuracy_records:
        p = output_root / "accuracy.csv"
        acc_fields = sorted({k for r in accuracy_records for k in r.keys()})
        with open(p, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=acc_fields, extrasaction="ignore")
            w.writeheader()
            w.writerows(accuracy_records)
        print(f"[OK] {p} ({len(accuracy_records)} rows)")

    # --- dataset_summary.csv ---
    # Map dataset -> list of candidate package paths to stat
    PACKAGE_PATHS = {
        "f3_amplitude": [
            "/mnt/f/CUP/erwt3d-paper/erwt3d/f3_amplitude_final.erwt3d",
        ],
        "f3_similarity": [
            "/mnt/f/CUP/erwt3d-paper/erwt3d/f3_similarity_final.erwt3d",
        ],
        "20GB": [
            "/mnt/f/CUP/erwt3d-paper/erwt3d/20GB_A0_auto_final.erwt3d",
            "/mnt/f/CUP/erwt3d-paper/erwt3d/20gb_plan_test.erwt3d",
        ],
    }
    ds_rows = []
    ds_info = {
        "f3_amplitude": {"shape": "201x201x51", "raw_bytes": 8241804,
                         "auto_format": "LZ4+XYZ", "lossless_or_lossy": "lossless"},
        "f3_similarity": {"shape": "191x146x51", "raw_bytes": 5688744,
                          "auto_format": "RZFP+XYZ", "lossless_or_lossy": "lossy"},
        "20GB": {"shape": "801x2405x2501", "raw_bytes": 19271755620,
                 "auto_format": "LZ4+XYZ", "lossless_or_lossy": "lossless"},
    }
    for ds_name, info in ds_info.items():
        # Stat actual package files on disk — NEVER rely on benchmark JSON fields
        pkg_bytes = 0
        pkg_path_found = None
        for candidate in PACKAGE_PATHS.get(ds_name, []):
            real = Path(candidate)
            if real.exists() and not real.is_symlink():
                sz = real.stat().st_size
                if sz > pkg_bytes:
                    pkg_bytes = sz
                    pkg_path_found = str(real)
            elif real.is_symlink():
                target = real.resolve()
                if target.exists():
                    sz = target.stat().st_size
                    if sz > pkg_bytes:
                        pkg_bytes = sz
                        pkg_path_found = str(target)
        if pkg_bytes == 0:
            print(f"[FAIL] {ds_name}: no package file found — cannot compute physical_storage_ratio")
        sr = round(pkg_bytes / info["raw_bytes"], 3) if info["raw_bytes"] and pkg_bytes else 0
        ds_rows.append({
            "dataset": ds_name, "shape": info["shape"],
            "raw_bytes": info["raw_bytes"],
            "auto_format": info["auto_format"],
            "final_package_bytes": pkg_bytes,
            "physical_storage_ratio": sr,
            "lossless_or_lossy": info["lossless_or_lossy"],
        })
    if ds_rows:
        p = output_root / "dataset_summary.csv"
        with open(p, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(ds_rows[0].keys()))
            w.writeheader()
            w.writerows(ds_rows)
        print(f"[OK] {p}")

    print(f"\n[DONE] Collected {len(all_records)} benchmark + {len(accuracy_records)} accuracy records into {output_root}")

if __name__ == "__main__":
    main()
