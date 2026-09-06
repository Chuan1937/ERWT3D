#!/usr/bin/env python3
"""Generate fixed random and continuous workloads for ERWT3D paper benchmark.

All methods use identical coordinates for fair comparison.
Outputs: validation/workloads/{random,continuous}_{x,y,z}.txt
"""

import os
import random
import sys

SEED = 42
N_RANDOM = 100
N_CONTINUOUS = 10

# Dataset dimensions (update after dataset generation)
# Format: {dataset_name: (nx, ny, nz)}
DATASETS = {
    "20GB": (801, 2405, 2501),   # small.dat dimensions
    "50GB": (2001, 2201, 3000),  # big.dat dimensions
}

def generate_random_coords(max_val, n, rng):
    """Generate n unique random indices in [0, max_val)."""
    return sorted(rng.sample(range(max_val), min(n, max_val)))

def generate_continuous_start(max_val, n_slices, rng):
    """Generate a random start index such that start + n_slices <= max_val."""
    return rng.randint(0, max(0, max_val - n_slices))

def write_list(path, values):
    with open(path, 'w') as f:
        for v in values:
            f.write(f"{v}\n")

def main():
    out_dir = os.path.join(os.path.dirname(__file__), '..', 'workloads')
    os.makedirs(out_dir, exist_ok=True)

    rng = random.Random(SEED)

    for ds_name, (nx, ny, nz) in DATASETS.items():
        ds_dir = os.path.join(out_dir, ds_name)
        os.makedirs(ds_dir, exist_ok=True)

        print(f"Dataset {ds_name}: nx={nx}, ny={ny}, nz={nz}")

        # Random coordinates
        rx = generate_random_coords(nx, N_RANDOM, rng)
        ry = generate_random_coords(ny, N_RANDOM, rng)
        rz = generate_random_coords(nz, N_RANDOM, rng)

        write_list(os.path.join(ds_dir, 'random_x.txt'), rx)
        write_list(os.path.join(ds_dir, 'random_y.txt'), ry)
        write_list(os.path.join(ds_dir, 'random_z.txt'), rz)

        print(f"  Random: {len(rx)} x-slices, {len(ry)} y-slices, {len(rz)} z-slices")

        # Continuous coordinates
        cx_start = generate_continuous_start(nx, N_CONTINUOUS, rng)
        cy_start = generate_continuous_start(ny, N_CONTINUOUS, rng)
        cz_start = generate_continuous_start(nz, N_CONTINUOUS, rng)

        cx = list(range(cx_start, cx_start + N_CONTINUOUS))
        cy = list(range(cy_start, cy_start + N_CONTINUOUS))
        cz = list(range(cz_start, cz_start + N_CONTINUOUS))

        write_list(os.path.join(ds_dir, 'continuous_x.txt'), cx)
        write_list(os.path.join(ds_dir, 'continuous_y.txt'), cy)
        write_list(os.path.join(ds_dir, 'continuous_z.txt'), cz)

        print(f"  Continuous: x=[{cx[0]}..{cx[-1]}], y=[{cy[0]}..{cy[-1]}], z=[{cz[0]}..{cz[-1]}]")

    print(f"\nSeed: {SEED}")
    print(f"Workloads written to: {out_dir}")

if __name__ == '__main__':
    main()
