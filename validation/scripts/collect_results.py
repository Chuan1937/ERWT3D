#!/usr/bin/env python3
"""Collect raw benchmark results into summary CSV files.

Reads all JSON files from raw_results/ and produces:
  - summaries/compression.csv
  - summaries/accuracy.csv
  - summaries/read_random.csv
  - summaries/read_continuous.csv
  - summaries/ablation.csv
  - summaries/geophysical_fidelity.csv
"""

import csv
import json
import os
import sys
from pathlib import Path

VALIDATION_DIR = Path(__file__).parent.parent
RAW_DIR = VALIDATION_DIR / "raw_results"
SUMMARY_DIR = VALIDATION_DIR / "summaries"

SUMMARY_DIR.mkdir(exist_ok=True)

def load_json_files(category):
    """Load all JSON files from a raw_results subdirectory."""
    results = []
    cat_dir = RAW_DIR / category
    if not cat_dir.exists():
        return results
    for f in sorted(cat_dir.glob("*.json")):
        with open(f) as fh:
            data = json.load(fh)
            data["_source_file"] = str(f.name)
            results.append(data)
    return results

def write_csv(path, rows, fieldnames=None):
    """Write rows to CSV. fieldnames from first row if not given."""
    if not rows:
        print(f"  [SKIP] No data for {path.name}")
        return
    if fieldnames is None:
        fieldnames = list(rows[0].keys())
    with open(path, 'w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames, extrasaction='ignore')
        writer.writeheader()
        writer.writerows(rows)
    print(f"  [OK] {path.name}: {len(rows)} rows")

def collect_compression():
    rows = load_json_files("compression")
    write_csv(SUMMARY_DIR / "compression.csv", rows)

def collect_accuracy():
    rows = load_json_files("accuracy")
    write_csv(SUMMARY_DIR / "accuracy.csv", rows)

def collect_random_read():
    rows = load_json_files("random_read")
    write_csv(SUMMARY_DIR / "read_random.csv", rows)

def collect_continuous_read():
    rows = load_json_files("continuous_read")
    write_csv(SUMMARY_DIR / "read_continuous.csv", rows)

def collect_ablation():
    rows = load_json_files("ablation")
    write_csv(SUMMARY_DIR / "ablation.csv", rows)

def collect_fidelity():
    rows = load_json_files("forward_modeling")
    write_csv(SUMMARY_DIR / "geophysical_fidelity.csv", rows)

def main():
    print("=== Collecting Benchmark Results ===")
    collect_compression()
    collect_accuracy()
    collect_random_read()
    collect_continuous_read()
    collect_ablation()
    collect_fidelity()
    print("\n=== Done ===")

if __name__ == '__main__':
    main()
