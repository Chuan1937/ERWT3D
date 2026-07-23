#!/usr/bin/env python3
# summarize_ssd_results.py — Parse contest_score.csv files and generate summary

import csv
import sys
import os
import json
from pathlib import Path

def parse_csv(path):
    d = {}
    with open(path, newline='') as f:
        reader = csv.reader(f)
        next(reader)  # skip header
        for row in reader:
            if len(row) >= 2:
                d[row[0]] = row[1]
    return d

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <results_dir>...")
        return 1

    for dirpath in sys.argv[1:]:
        path = Path(dirpath) / "contest_score.csv"
        if not path.exists():
            print(f"SKIP: {path} not found")
            continue

        d = parse_csv(str(path))
        t_comp = float(d.get("T_composite_ms", 0))
        fmt = d.get("format", "unknown")
        io_prof = d.get("requested_io_profile", "unknown")
        resolved = d.get("resolved_io_profile", "unknown")
        ratio = float(d.get("storage_ratio", 0))

        print(f"\n--- {dirpath} ---")
        print(f"  Format:      {fmt}")
        print(f"  IO Profile:  {io_prof} -> {resolved}")
        print(f"  T_composite: {t_comp / 1000:.3f} s")
        print(f"  Storage:     {ratio:.3f}x")
        print(f"  WSL:         {d.get('wsl_detected', 'unknown')}")
        print(f"  FS:          {d.get('filesystem_type', 'unknown')}")

    return 0

if __name__ == "__main__":
    sys.exit(main())
