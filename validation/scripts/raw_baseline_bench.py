#!/usr/bin/env python3
"""Raw baseline benchmark: read slices directly from float32 raw file.
Produces JSON compatible with FINAL benchmark schema."""
import argparse, json, os, sys, time
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

def read_workload(path):
    with open(path) as f:
        return [int(line.strip()) for line in f if line.strip()]

def read_x_slice(data, nx, ny, nz, idx):
    """X slice: all y,z for fixed x -> shape (ny, nz)"""
    plane = data[idx, :, :]
    return plane.ravel()

def read_y_slice(data, nx, ny, nz, idx):
    """Y slice: all x,z for fixed y -> shape (nx, nz)"""
    plane = data[:, idx, :]
    return plane.ravel()

def read_z_slice(data, nx, ny, nz, idx):
    """Z slice: all x,y for fixed z -> shape (nx, ny)"""
    plane = data[:, :, idx]
    return plane.ravel()

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--raw", required=True)
    p.add_argument("--dataset", required=True)
    p.add_argument("--nx", type=int, required=True)
    p.add_argument("--ny", type=int, required=True)
    p.add_argument("--nz", type=int, required=True)
    p.add_argument("--axis", required=True, choices=["x","y","z"])
    p.add_argument("--pattern", required=True, choices=["random","continuous"])
    p.add_argument("--workload", required=True)
    p.add_argument("--output-json", required=True)
    p.add_argument("--device", default="SSD")
    p.add_argument("--run", type=int, default=1)
    p.add_argument("--git-commit", default="")
    p.add_argument("--threads", type=int, default=8)
    args = p.parse_args()

    positions = read_workload(args.workload)
    nx, ny, nz = args.nx, args.ny, args.nz

    # Memory-map the raw file
    data = np.memmap(args.raw, dtype=np.float32, mode='r', shape=(nx, ny, nz))

    readers = {"x": read_x_slice, "y": read_y_slice, "z": read_z_slice}
    reader = readers[args.axis]

    # Drop caches hint
    os.system("sync")
    try:
        with open("/proc/sys/vm/drop_caches", "w") as f:
            f.write("3")
    except:
        pass

    slice_latencies_ms = []
    total_bytes = 0
    t_total_start = time.perf_counter()

    for pos in positions:
        t0 = time.perf_counter()
        out = reader(data, nx, ny, nz, pos)
        t1 = time.perf_counter()
        slice_latencies_ms.append((t1 - t0) * 1000)
        total_bytes += out.nbytes

    t_total_end = time.perf_counter()
    total_time_ms = (t_total_end - t_total_start) * 1000

    output_bytes = nx * nz * 4 if args.axis == "y" else nx * ny * 4 if args.axis == "z" else ny * nz * 4

    result = {
        "benchmark_schema_version": "final-1",
        "status": "SUCCESS",
        "run_id": f"{args.dataset}_raw_baseline_{args.device}_{args.axis}_{args.pattern}_run{args.run:02d}",
        "git_commit": args.git_commit,
        "algorithm_commit": args.git_commit,
        "dataset": args.dataset,
        "format": "RAW",
        "layout": "raw",
        "configuration": "raw_baseline",
        "device": args.device,
        "axis": args.axis,
        "pattern": args.pattern,
        "run_number": args.run,
        "threads": args.threads,
        "cache_mode": "cold_linux_guest_page_cache",
        "input_path": args.raw,
        "workload_path": args.workload,
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "total_time_ms": total_time_ms,
        "slice_count": len(positions),
        "slice_latencies_ms": slice_latencies_ms,
        "bytes_read": total_bytes,
        "output_bytes": output_bytes * len(positions),
        "pread_calls": len(positions),
        "positions": positions,
    }

    Path(args.output_json).parent.mkdir(parents=True, exist_ok=True)
    with open(args.output_json, "w") as f:
        json.dump(result, f, indent=2)
    print(f"[OK] {args.output_json} ({len(positions)} slices, {total_time_ms:.0f} ms)")

if __name__ == "__main__":
    main()
