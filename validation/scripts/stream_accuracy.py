#!/usr/bin/env python3
"""Streaming reconstruction-accuracy evaluation for float32 raw volumes."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--raw", type=Path, required=True)
    parser.add_argument("--restored", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--chunk-mib", type=int, default=128)
    parser.add_argument("--zero-abs-tol", type=float, default=1e-6)
    parser.add_argument("--relative-bound", type=float, default=1e-3)
    parser.add_argument("--quantiles", choices=("exact", "sample"), default="exact")
    args = parser.parse_args()
    if args.raw.stat().st_size != args.restored.stat().st_size:
        raise SystemExit("raw and restored sizes differ")
    values_per_chunk = max(1, args.chunk_mib * 1024 * 1024 // 4)
    count = violations = zero_total = zero_exact = nan_ok = inf_ok = 0
    sum_sq_error = sum_sq_raw = sum_relative = 0.0
    max_abs = max_relative = 0.0
    relative_values: list[np.ndarray] = []
    with args.raw.open("rb") as raw, args.restored.open("rb") as restored:
        while True:
            original = np.frombuffer(raw.read(values_per_chunk * 4), dtype=np.float32)
            decoded = np.frombuffer(restored.read(values_per_chunk * 4), dtype=np.float32)
            if original.size == 0:
                break
            if original.size != decoded.size:
                raise SystemExit("unexpected short read")
            count += original.size
            nan_mask = np.isnan(original)
            inf_mask = np.isinf(original)
            nan_ok += int(np.equal(nan_mask, np.isnan(decoded)).sum())
            inf_ok += int(np.equal(inf_mask, np.isinf(decoded)).sum())
            finite = np.isfinite(original) & np.isfinite(decoded)
            a = original[finite].astype(np.float64)
            b = decoded[finite].astype(np.float64)
            error = np.abs(a - b)
            max_abs = max(max_abs, float(error.max(initial=0.0)))
            sum_sq_error += float(np.dot(a - b, a - b))
            sum_sq_raw += float(np.dot(a, a))
            zero = np.abs(a) <= args.zero_abs_tol
            zero_total += int(zero.sum())
            zero_exact += int(np.logical_and(zero, a == b).sum())
            nonzero_error = error[~zero] / np.abs(a[~zero])
            if nonzero_error.size:
                max_relative = max(max_relative, float(nonzero_error.max()))
                sum_relative += float(nonzero_error.sum())
                violations += int((nonzero_error >= args.relative_bound).sum())
                relative_values.append(nonzero_error)
    all_relative = np.concatenate(relative_values) if relative_values else np.array([], dtype=np.float64)
    if args.quantiles != "exact":
        raise SystemExit("sampling quantiles is not implemented; do not label samples as exact")
    payload = {
        "status": "SUCCESS", "quantile_method": "exact", "elements": count,
        "max_absolute_error": max_abs,
        "rmse": math.sqrt(sum_sq_error / count) if count else 0.0,
        "nrmse": math.sqrt(sum_sq_error / sum_sq_raw) if sum_sq_raw else 0.0,
        "max_relative_error": max_relative,
        "mean_relative_error": sum_relative / all_relative.size if all_relative.size else 0.0,
        "p95_relative_error": float(np.percentile(all_relative, 95)) if all_relative.size else 0.0,
        "p99_relative_error": float(np.percentile(all_relative, 99)) if all_relative.size else 0.0,
        "p999_relative_error": float(np.percentile(all_relative, 99.9)) if all_relative.size else 0.0,
        "zero_exact_preservation_rate": zero_exact / zero_total if zero_total else 1.0,
        "nan_mask_preserved_count": nan_ok, "inf_mask_preserved_count": inf_ok,
        "relative_error_violations": violations, "relative_bound": args.relative_bound,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(payload, indent=2) + "\n")
    print(json.dumps(payload, indent=2))


if __name__ == "__main__":
    main()
