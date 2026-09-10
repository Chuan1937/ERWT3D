#!/usr/bin/env python3
"""Aggregate successful raw JSON records without hiding failed runs."""
from __future__ import annotations
import csv, json, math, statistics
from collections import defaultdict
from pathlib import Path

VALIDATION = Path(__file__).resolve().parents[1]

def scalar_row(data: dict, source: Path) -> dict:
    row = {k: v for k, v in data.items() if isinstance(v, (str, int, float, bool)) or v is None}
    row["source_file"] = source.name
    return row

def percentile(values: list[float], q: float) -> float:
    values = sorted(values)
    if not values: return float("nan")
    i = (len(values) - 1) * q; low, high = math.floor(i), math.ceil(i)
    return values[low] if low == high else values[low] + (values[high] - values[low]) * (i - low)

def write_csv(path: Path, rows: list[dict]) -> None:
    fields = sorted({k for row in rows for k in row})
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields); writer.writeheader(); writer.writerows(rows)

def main() -> None:
    raw, summaries = VALIDATION / "raw_results", VALIDATION / "summaries"
    records = []
    for path in sorted(raw.glob("**/*.json")):
        try: data = json.loads(path.read_text())
        except json.JSONDecodeError as error:
            print(f"[INVALID] {path}: {error}"); continue
        data["_category"] = path.parent.name; records.append((data, path))
    categories: dict[str, list[dict]] = defaultdict(list); successful = []
    for data, path in records:
        row = scalar_row(data, path); categories[data["_category"]].append(row)
        if data.get("status", "SUCCESS") == "SUCCESS" and int(data.get("exit_code", 0)) == 0: successful.append(row)
    for category, rows in categories.items(): write_csv(summaries / f"{category}_runs.csv", rows)
    groups: dict[tuple, list[float]] = defaultdict(list)
    for row in successful:
        if isinstance(row.get("total_time_ms"), (int, float)):
            groups[(row.get("_category"), row.get("dataset"), row.get("method", "ERWT3D"), row.get("device"), row.get("axis"), row.get("pattern"))].append(float(row["total_time_ms"]))
    aggregate = []
    for key, values in sorted(groups.items(), key=lambda item: str(item[0])):
        mean = statistics.fmean(values); std = statistics.stdev(values) if len(values) > 1 else 0.0; cv = std / mean * 100 if mean else 0.0
        aggregate.append({"category": key[0], "dataset": key[1], "method": key[2], "device": key[3], "axis": key[4], "pattern": key[5], "n": len(values), "mean_ms": mean, "std_ms": std, "cv_percent": cv, "median_ms": percentile(values, .5), "p95_ms": percentile(values, .95), "p99_ms": percentile(values, .99), "stability": "UNSTABLE" if cv > 5 else "STABLE"})
    write_csv(summaries / "benchmark_aggregate.csv", aggregate)
    print(f"[OK] discovered={len(records)} successful={len(successful)} aggregate_groups={len(aggregate)}")

if __name__ == "__main__": main()
