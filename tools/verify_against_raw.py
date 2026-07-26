#!/usr/bin/env python3
"""Compare cold executor 330 outputs against original raw DAT."""
import struct, os, sys, numpy as np

RAW = "/mnt/g/cup/cup_3d_big.dat"
COLD_DIR = "/home/chuan/ssd_correctness_pr64/cold"
POS_FILE = "/home/chuan/code/ERWT3D/positions/positions_big.csv"
NX, NY, NZ = 2001, 2201, 3000

raw = np.memmap(RAW, dtype="<f4", mode="r", shape=(NX, NY, NZ), order="C")

def read_plane(axis, idx):
    if axis == "x":
        return np.asarray(raw[idx, :, :])
    elif axis == "y":
        return np.asarray(raw[:, idx, :])
    else:
        return np.asarray(raw[:, :, idx])

counters = {"x_random": 0, "y_random": 0, "z_random": 0,
            "x_continuous": 0, "y_continuous": 0, "z_continuous": 0}

total_elements = 0
violations = 0
zero_mismatch = 0
nonfinite_mismatch = 0
max_re = 0.0

slot = 0
with open(POS_FILE) as f:
    for line in f:
        if not line.strip() or line.startswith("axis"):
            continue
        parts = line.strip().split(",")
        if len(parts) != 3: continue
        axis, typ, idx = parts[0], parts[1], int(parts[2])
        key = f"{axis}_{typ}"
        seq = counters[key]
        counters[key] += 1
        fname = f"contest_{key}_{seq:03d}.dat"
        path = os.path.join(COLD_DIR, fname)
        if not os.path.exists(path):
            print(f"MISSING: {fname}")
            sys.exit(1)

        cold_data = np.fromfile(path, dtype="<f4")
        raw_data = read_plane(axis, idx).ravel()
        assert cold_data.shape == raw_data.shape, f"Shape mismatch {fname}"

        mask_finite = np.isfinite(raw_data)
        re = np.zeros_like(raw_data)
        denom = np.abs(raw_data) + 1e-30
        re[mask_finite] = np.abs(cold_data[mask_finite] - raw_data[mask_finite]) / denom[mask_finite]
        v = int(np.sum(re[mask_finite] >= 0.001))
        violations += v

        mr = float(np.max(re[mask_finite])) if mask_finite.any() else 0.0
        max_re = max(max_re, mr)

        zm = int(np.sum((raw_data == 0.0) & (cold_data != 0.0)))
        zm += int(np.sum((cold_data == 0.0) & (raw_data != 0.0)))
        zero_mismatch += zm

        nf = int(np.sum(np.isfinite(raw_data) != np.isfinite(cold_data)))
        nonfinite_mismatch += nf

        total_elements += cold_data.size
        slot += 1

        if slot <= 5 or slot % 100 == 0:
            print(f"  {fname}: max_re={mr:.6e} viol={v} zm={zm}")
            sys.stdout.flush()

        if slot >= 330:
            break

print(f"\n=== Final Results ===")
print(f"files_checked={slot}")
print(f"elements_checked={total_elements}")
print(f"relative_error_violations={violations}")
print(f"zero_mismatches={zero_mismatch}")
print(f"nonfinite_mismatches={nonfinite_mismatch}")
print(f"global_max_relative_error={max_re:.6e}")
print(f"verdict={'PASS' if violations == 0 and zero_mismatch == 0 and nonfinite_mismatch == 0 else 'FAIL'}")
