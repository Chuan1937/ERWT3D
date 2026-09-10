#!/usr/bin/env python3
"""Convert a SEG-Y cube to the ERWT3D external float32 X-Y-Z raw layout."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import numpy as np

VALIDATION = Path(__file__).resolve().parents[2]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def streamed_stats(path: Path, chunk_bytes: int = 128 << 20) -> dict[str, float | int]:
    count = nan_count = inf_count = zero_count = 0
    total = total_sq = 0.0
    minimum = float("inf")
    maximum = float("-inf")
    with path.open("rb") as stream:
        while True:
            block = np.frombuffer(stream.read(chunk_bytes), dtype=np.float32)
            if block.size == 0:
                break
            finite = block[np.isfinite(block)]
            count += block.size
            nan_count += int(np.isnan(block).sum())
            inf_count += int(np.isinf(block).sum())
            zero_count += int((block == 0).sum())
            if finite.size:
                values = finite.astype(np.float64)
                total += float(values.sum())
                total_sq += float(np.dot(values, values))
                minimum = min(minimum, float(values.min()))
                maximum = max(maximum, float(values.max()))
    finite_count = count - nan_count - inf_count
    mean = total / finite_count if finite_count else float("nan")
    variance = max(0.0, total_sq / finite_count - mean * mean) if finite_count else float("nan")
    return {"min": minimum, "max": maximum, "mean": mean, "std": variance ** 0.5,
            "zero_fraction": zero_count / count if count else 0.0,
            "nan_count": nan_count, "inf_count": inf_count, "elements": count}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True, help="Source SEG-Y file")
    parser.add_argument("--dataset-id", required=True)
    parser.add_argument("--output-dir", type=Path, default=VALIDATION / "datasets" / "prepared")
    parser.add_argument("--license", default="CC-BY-SA")
    parser.add_argument("--source", default="")
    args = parser.parse_args()
    try:
        import segyio
    except ImportError as exc:
        raise SystemExit("segyio is required: python3 -m pip install segyio") from exc
    if not args.input.is_file():
        raise SystemExit(f"missing SEG-Y input: {args.input}")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    output = args.output_dir / f"{args.dataset_id}.raw"
    with segyio.open(str(args.input), "r", ignore_geometry=False) as segy:
        cube = segyio.tools.cube(segy)
    data = np.ascontiguousarray(cube, dtype=np.float32)
    if data.ndim != 3:
        raise SystemExit(f"expected a 3-D SEG-Y cube, got shape {data.shape}")
    data.tofile(output)
    stats = streamed_stats(output)
    metadata = {
        "dataset_id": args.dataset_id,
        "source_format": "SEG-Y",
        "prepared_format": "float32_raw",
        "shape": list(data.shape),
        "axis_order": "xyz",
        "fastest_axis": "z",
        "source_sha256": sha256(args.input),
        "prepared_sha256": sha256(output),
        "source_bytes": args.input.stat().st_size,
        "prepared_bytes": output.stat().st_size,
        "source": args.source or str(args.input),
        "license": args.license,
        **stats,
    }
    metadata_path = args.output_dir / f"{args.dataset_id}.metadata.json"
    metadata_path.write_text(json.dumps(metadata, indent=2, allow_nan=False) + "\n")
    print(f"[OK] {output}: shape={tuple(data.shape)}")
    print(f"[OK] {metadata_path}")


if __name__ == "__main__":
    main()
