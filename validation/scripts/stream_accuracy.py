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
    parser.add_argument("--sample-size", type=int, default=10_000_000,
                        help="Deterministic element sample size when --quantiles sample")
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args()
    if args.raw.stat().st_size != args.restored.stat().st_size:
        raise SystemExit("raw and restored sizes differ")
    values_per_chunk = max(1, args.chunk_mib * 1024 * 1024 // 4)
    total_elements = args.raw.stat().st_size // 4
    if args.quantiles == "sample":
        if args.sample_size < 1:
            raise SystemExit("--sample-size must be positive")
        rng = np.random.default_rng(args.seed)
        sampled_indices = np.sort(rng.integers(
            0, total_elements, size=min(args.sample_size, total_elements), endpoint=False
        ))
    else:
        sampled_indices = None
    count = violations = zero_total = zero_exact = nan_ok = inf_ok = 0
    sum_sq_error = sum_sq_raw = sum_relative = 0.0
    max_abs = max_relative = 0.0
    relative_values: list[np.ndarray] = []
    relative_count = 0
    offset = 0
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
                relative_count += int(nonzero_error.size)
                violations += int((nonzero_error >= args.relative_bound).sum())
            if args.quantiles == "exact":
                relative_values.append(nonzero_error)
            else:
                left = np.searchsorted(sampled_indices, offset, side="left")
                right = np.searchsorted(sampled_indices, offset + original.size, side="left")
                if right > left:
                    local = sampled_indices[left:right] - offset
                    sample_original = original[local].astype(np.float64)
                    sample_decoded = decoded[local].astype(np.float64)
                    valid = np.isfinite(sample_original) & np.isfinite(sample_decoded) & (np.abs(sample_original) > args.zero_abs_tol)
                    if np.any(valid):
                        relative_values.append(np.abs(sample_original[valid] - sample_decoded[valid]) / np.abs(sample_original[valid]))
            offset += original.size
    all_relative = np.concatenate(relative_values) if relative_values else np.array([], dtype=np.float64)
    payload = {
        "status": "SUCCESS",
        "quantile_method": "exact" if args.quantiles == "exact" else "deterministic_sample",
        "quantile_sample_size": int(all_relative.size) if args.quantiles == "sample" else None,
        "quantile_seed": args.seed if args.quantiles == "sample" else None,
        "elements": count,
        "max_absolute_error": max_abs,
        "rmse": math.sqrt(sum_sq_error / count) if count else 0.0,
        "nrmse": math.sqrt(sum_sq_error / sum_sq_raw) if sum_sq_raw else 0.0,
        "max_relative_error": max_relative,
        "mean_relative_error": sum_relative / relative_count if relative_count else 0.0,
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
