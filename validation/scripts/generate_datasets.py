#!/usr/bin/env python3
"""Generate synthetic datasets for ERWT3D paper experiments.

Dataset A: Velocity model — smooth 3D field with high spatial correlation
Dataset B: Seismic volume — band-limited with local continuity
Dataset C: Wavefield snapshot — wavefront structures
Dataset D: Random control — Gaussian noise, weak correlation

All outputs: float32, raw binary, X-Y-Z row-major (Z fastest).
"""

import argparse
import hashlib
import json
import math
import os
import struct
import sys
import csv
from pathlib import Path

import numpy as np

VALIDATION_DIR = Path(__file__).parent.parent
DATASETS_DIR = VALIDATION_DIR / "datasets"
RAW_DIR = DATASETS_DIR / "raw"

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()

def write_raw(path, data):
    data.astype(np.float32).tofile(path)

def dataset_stats(data):
    """Compute statistics for a float32 array."""
    flat = data.ravel().astype(np.float64)
    return {
        "dtype": "float32",
        "min": float(np.nanmin(flat)),
        "max": float(np.nanmax(flat)),
        "mean": float(np.nanmean(flat)),
        "std": float(np.nanstd(flat)),
        "zero_fraction": float(np.sum(flat == 0) / flat.size),
        "nan_count": int(np.sum(np.isnan(flat))),
        "inf_count": int(np.sum(np.isinf(flat))),
        "elements": int(flat.size),
        "raw_size_bytes": int(flat.size * 4),
    }

def gen_velocity_model(nx, ny, nz, seed=42):
    """Dataset A: Smooth velocity model via filtered random field."""
    rng = np.random.RandomState(seed)
    # Base smooth field using superposition of low-frequency harmonics
    x = np.linspace(0, 4 * np.pi, nx, dtype=np.float32)
    y = np.linspace(0, 4 * np.pi, ny, dtype=np.float32)
    z = np.linspace(0, 4 * np.pi, nz, dtype=np.float32)
    X, Y, Z = np.meshgrid(x, y, z, indexing='ij')

    data = (2000.0
            + 500.0 * np.sin(X * 0.5) * np.cos(Y * 0.3)
            + 300.0 * np.sin(Z * 0.7 + 1.0)
            + 200.0 * np.cos(X * 0.2 + Y * 0.4)
            + rng.randn(nx, ny, nz).astype(np.float32) * 50.0)
    return data.astype(np.float32)

def gen_seismic_volume(nx, ny, nz, seed=42):
    """Dataset B: Band-limited seismic volume."""
    rng = np.random.RandomState(seed)
    # Ricker wavelet convolved reflectivity
    reflectivity = rng.randn(nx, ny, nz).astype(np.float32) * 0.5

    # Add layered structure
    for iz in range(nz):
        if iz % 40 < 3:
            reflectivity[:, :, iz] += 1.0

    # Simple smoothing to simulate band-limited data
    from scipy.ndimage import gaussian_filter
    data = gaussian_filter(reflectivity, sigma=[2, 2, 1.5])
    # Scale to realistic amplitude range
    data = data * 1000.0 + 1500.0
    return data.astype(np.float32)

def gen_wavefield(nx, ny, nz, seed=42):
    """Dataset C: Wavefield with wavefront structures."""
    x = np.linspace(-5, 5, nx, dtype=np.float32)
    y = np.linspace(-5, 5, ny, dtype=np.float32)
    z = np.linspace(-5, 5, nz, dtype=np.float32)
    X, Y, Z = np.meshgrid(x, y, z, indexing='ij')

    R = np.sqrt(X**2 + Y**2 + Z**2)
    # Expanding wavefront
    data = (np.sin(3.0 * R) * np.exp(-0.1 * R**2)
            + 0.3 * np.sin(5.0 * X + 2.0 * Y) * np.cos(3.0 * Z))
    return data.astype(np.float32) * 1000.0

def gen_random_control(nx, ny, nz, seed=42):
    """Dataset D: Gaussian random control."""
    rng = np.random.RandomState(seed)
    return rng.randn(nx, ny, nz).astype(np.float32)

def main():
    parser = argparse.ArgumentParser(description="Generate ERWT3D paper datasets")
    parser.add_argument("--sizes", nargs="+", default=["small"],
                        choices=["tiny", "small", "medium", "large"],
                        help="Dataset sizes to generate")
    parser.add_argument("--datasets", nargs="+", default=["A", "B", "C", "D"],
                        choices=["A", "B", "C", "D"],
                        help="Which datasets to generate")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    # Dimensions for each size tier
    SIZE_DIMS = {
        "tiny":   (100, 120, 130),    # ~5 MB
        "small":  (200, 250, 260),     # ~50 MB
        "medium": (400, 500, 520),     # ~400 MB
        "large":  (801, 2405, 2501),   # ~18 GB (matches competition small.dat)
    }

    GENERATORS = {
        "A": ("velocity", gen_velocity_model),
        "B": ("seismic",  gen_seismic_volume),
        "C": ("wavefield", gen_wavefield),
        "D": ("random",   gen_random_control),
    }

    RAW_DIR.mkdir(parents=True, exist_ok=True)
    manifest = []

    for size_name in args.sizes:
        nx, ny, nz = SIZE_DIMS[size_name]
        size_bytes = nx * ny * nz * 4
        size_gb = size_bytes / (1024**3)

        for ds_id in args.datasets:
            ds_type, gen_fn = GENERATORS[ds_id]
            filename = f"dataset{ds_id}_{ds_type}_{size_name}.raw"
            filepath = RAW_DIR / filename

            print(f"\n=== Dataset {ds_id} ({ds_type}) — {size_name} ===")
            print(f"  Dimensions: {nx} x {ny} x {nz}")
            print(f"  Raw size: {size_bytes / 1024**2:.1f} MB ({size_gb:.3f} GB)")
            print(f"  Output: {filepath}")

            if args.dry_run:
                print("  [DRY RUN] Skipping generation")
                continue

            if filepath.exists():
                print("  [SKIP] File already exists")
            else:
                print("  Generating...")
                data = gen_fn(nx, ny, nz, seed=args.seed)
                write_raw(filepath, data)
                print("  [OK] Written")

            # Compute stats on a sample (avoid loading 18GB for stats)
            if size_gb < 2.0:
                data = np.fromfile(str(filepath), dtype=np.float32).reshape(nx, ny, nz)
                stats = dataset_stats(data)
            else:
                # Sample-based stats for large datasets
                sample = np.fromfile(str(filepath), dtype=np.float32, count=10**7)
                stats = dataset_stats(sample)
                stats["note"] = "sampled from first 10M elements"

            sha = sha256_file(filepath)
            stats["sha256"] = sha
            stats["file"] = str(filepath)
            stats["dimensions"] = f"{nx}x{ny}x{nz}"

            # Write per-dataset stats
            stats_path = RAW_DIR / f"dataset{ds_id}_{ds_type}_{size_name}_stats.json"
            with open(stats_path, 'w') as f:
                json.dump(stats, f, indent=2)
            print(f"  Stats: {stats_path}")

            manifest.append({
                "dataset_id": f"dataset{ds_id}",
                "type": ds_type,
                "size_tier": size_name,
                "nx": nx, "ny": ny, "nz": nz,
                "filename": filename,
                "sha256": sha,
                **{k: v for k, v in stats.items() if k not in ("file", "dimensions", "sha256", "note")},
            })

    # Write manifest
    if manifest and not args.dry_run:
        manifest_path = DATASETS_DIR / "manifest.csv"
        fieldnames = list(manifest[0].keys())
        with open(manifest_path, 'w', newline='') as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            writer.writerows(manifest)
        print(f"\n[OK] Manifest: {manifest_path}")

        # SHA256 file
        sha_path = DATASETS_DIR / "checksums.sha256"
        with open(sha_path, 'w') as f:
            for entry in manifest:
                f.write(f"{entry['sha256']}  {entry['filename']}\n")
        print(f"[OK] Checksums: {sha_path}")

if __name__ == '__main__':
    main()
