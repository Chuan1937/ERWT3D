#!/usr/bin/env python3
"""Generate versioned, deterministic per-dataset slice workloads.

Random lists deliberately retain sampling order. Sorting them would change a
random-access workload into an increasingly sequential one.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import random
from datetime import datetime, timezone
from pathlib import Path

VALIDATION = Path(__file__).resolve().parents[1]
DEFAULT_DATASETS = {"20GB": (801, 2405, 2501), "50GB": (2001, 2201, 3000)}


def checksum(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_indices(path: Path, values: list[int]) -> str:
    path.write_text("".join(f"{value}\n" for value in values))
    return checksum(path)


def dimensions_from_metadata(path: Path) -> tuple[int, int, int]:
    metadata = json.loads(path.read_text())
    shape = metadata.get("shape")
    if not isinstance(shape, list) or len(shape) != 3 or any(not isinstance(x, int) or x <= 0 for x in shape):
        raise SystemExit(f"invalid shape in {path}")
    return tuple(shape)  # type: ignore[return-value]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset", action="append", help="ID=NX,NY,NZ; may be repeated")
    parser.add_argument("--metadata", type=Path, action="append", help="Prepared dataset metadata JSON")
    parser.add_argument("--output-dir", type=Path, default=VALIDATION / "workloads")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--random-count", type=int, default=100)
    parser.add_argument("--continuous-count", type=int, default=10)
    args = parser.parse_args()
    datasets = dict(DEFAULT_DATASETS) if not args.dataset and not args.metadata else {}
    for spec in args.dataset or []:
        try:
            dataset_id, dims = spec.split("=", 1)
            values = tuple(int(x) for x in dims.split(","))
        except ValueError as exc:
            raise SystemExit(f"invalid --dataset {spec!r}; expected ID=NX,NY,NZ") from exc
        if len(values) != 3 or min(values) <= 0:
            raise SystemExit(f"invalid dimensions in {spec!r}")
        datasets[dataset_id] = values
    for metadata in args.metadata or []:
        datasets[metadata.name.removesuffix(".metadata.json")] = dimensions_from_metadata(metadata)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    for dataset_id, dimensions in sorted(datasets.items()):
        target = args.output_dir / dataset_id
        target.mkdir(parents=True, exist_ok=True)
        rng = random.Random(f"{args.seed}:{dataset_id}")
        file_checksums: dict[str, str] = {}
        for axis, limit in zip("xyz", dimensions):
            random_indices = rng.sample(range(limit), min(args.random_count, limit))
            if limit < args.continuous_count:
                raise SystemExit(f"{dataset_id}: {axis} dimension smaller than continuous count")
            start = rng.randrange(limit - args.continuous_count + 1)
            continuous_indices = list(range(start, start + args.continuous_count))
            file_checksums[f"random_{axis}.txt"] = write_indices(target / f"random_{axis}.txt", random_indices)
            file_checksums[f"continuous_{axis}.txt"] = write_indices(target / f"continuous_{axis}.txt", continuous_indices)
        metadata = {
            "dataset_id": dataset_id, "dimensions": list(dimensions), "axis_order": "xyz",
            "seed": args.seed, "random_count": args.random_count,
            "continuous_count": args.continuous_count,
            "generated_at_utc": datetime.now(timezone.utc).isoformat(),
            "random_order": "sampled_order_preserved", "sha256": file_checksums,
        }
        (target / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
        print(f"[OK] {dataset_id}: {dimensions} -> {target}")


if __name__ == "__main__":
    main()
