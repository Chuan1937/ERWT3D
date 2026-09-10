#!/usr/bin/env python3
"""Generate small synthetic controls; never label them as real seismic data.

Large competition-scale data are deliberately rejected. Their use belongs to
the supplied formal benchmark datasets, not a memory-heavy Python generator.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import numpy as np

VALIDATION = Path(__file__).resolve().parents[1]
RAW_DIR = VALIDATION / "datasets" / "raw"
DATASETS = {
    "synthetic_smooth_model": "smooth analytic velocity-like control",
    "synthetic_correlated_volume": "locally correlated synthetic volume",
    "synthetic_wavefield": "analytic wavefront control",
    "random_control": "independent Gaussian control",
}
SIZES = {"tiny": (100, 120, 130), "small": (200, 250, 260), "medium": (400, 500, 520)}


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def generate_chunk(dataset: str, x_count: int, ny: int, nz: int, x0: int, full_nx: int,
                   rng: np.random.Generator) -> np.ndarray:
    x = np.arange(x0, x0 + x_count, dtype=np.float32)[:, None, None] / max(1, full_nx - 1)
    y = np.arange(ny, dtype=np.float32)[None, :, None] / max(1, ny - 1)
    z = np.arange(nz, dtype=np.float32)[None, None, :] / max(1, nz - 1)
    if dataset == "synthetic_smooth_model":
        result = 2000 + 500 * np.sin(2 * np.pi * x) * np.cos(1.2 * np.pi * y) + 300 * np.sin(1.4 * np.pi * z)
    elif dataset == "synthetic_correlated_volume":
        result = 1500 + 300 * np.sin(8 * np.pi * z + 2 * np.pi * x) + 100 * np.cos(4 * np.pi * y)
    elif dataset == "synthetic_wavefield":
        radius = np.sqrt((x - .5) ** 2 + (y - .5) ** 2 + (z - .5) ** 2)
        result = 1000 * np.sin(30 * radius) * np.exp(-3 * radius)
    else:
        result = rng.standard_normal((x_count, ny, nz), dtype=np.float32)
    if dataset != "random_control":
        result = result + rng.standard_normal(result.shape, dtype=np.float32) * 2
    return np.ascontiguousarray(result, dtype=np.float32)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sizes", nargs="+", default=["small"], choices=tuple(SIZES))
    parser.add_argument("--datasets", nargs="+", default=list(DATASETS), choices=tuple(DATASETS))
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--x-chunk", type=int, default=16)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    RAW_DIR.mkdir(parents=True, exist_ok=True)
    for size_name in args.sizes:
        nx, ny, nz = SIZES[size_name]
        for dataset in args.datasets:
            output = RAW_DIR / f"{dataset}_{size_name}.raw"
            print(f"[PLAN] {dataset}: {nx}x{ny}x{nz} -> {output}")
            if args.dry_run:
                continue
            rng = np.random.default_rng(args.seed)
            with output.open("wb") as stream:
                for x0 in range(0, nx, args.x_chunk):
                    generate_chunk(dataset, min(args.x_chunk, nx - x0), ny, nz, x0, nx, rng).tofile(stream)
            metadata = {"dataset_id": dataset, "scientific_type": "synthetic_control",
                        "description": DATASETS[dataset], "shape": [nx, ny, nz],
                        "axis_order": "xyz", "fastest_axis": "z", "seed": args.seed,
                        "prepared_sha256": sha256(output), "prepared_bytes": output.stat().st_size}
            (RAW_DIR / f"{dataset}_{size_name}.metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")


if __name__ == "__main__":
    main()
