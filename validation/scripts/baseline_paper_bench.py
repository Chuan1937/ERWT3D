#!/usr/bin/env python3
"""Single-axis Raw/HDF5 benchmark with the ERWT3D paper-metric schema."""
from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

import numpy as np


def positions(path: Path, limit: int, continuous: bool) -> list[int]:
    values = [int(line.split("#", 1)[0].strip()) for line in path.read_text().splitlines() if line.split("#", 1)[0].strip()]
    if not values or any(value < 0 or value >= limit for value in values) or len(set(values)) != len(values):
        raise SystemExit("positions must be nonempty, unique, and in range")
    if continuous and any(current != previous + 1 for previous, current in zip(values, values[1:])):
        raise SystemExit("continuous positions must be consecutive in file order")
    return values


def slice_view(volume, axis: str, index: int):
    return volume[index, :, :] if axis == "x" else volume[:, index, :] if axis == "y" else volume[:, :, index]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--method", choices=("raw", "hdf5_raw", "hdf5_gzip"), required=True)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--shape", nargs=3, type=int, required=True)
    parser.add_argument("--axis", choices=("x", "y", "z"), required=True)
    parser.add_argument("--pattern", choices=("random", "continuous"), required=True)
    parser.add_argument("--positions-file", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--metrics-json", type=Path, required=True)
    args = parser.parse_args()
    shape = tuple(args.shape)
    axis_index = "xyz".index(args.axis)
    values = positions(args.positions_file, shape[axis_index], args.pattern == "continuous")
    if args.method == "raw":
        volume = np.memmap(args.input, dtype=np.float32, mode="r", shape=shape, order="C")
        input_format = "float32_raw_xyz"
    else:
        try:
            import h5py
        except ImportError as error:
            raise SystemExit("h5py is required for HDF5 baselines") from error
        handle = h5py.File(args.input, "r")
        volume = handle["volume"]
        if tuple(volume.shape) != shape or volume.dtype != np.float32:
            raise SystemExit("HDF5 dataset 'volume' must be float32 with the declared shape")
        input_format = args.method
    args.output_dir.mkdir(parents=True, exist_ok=True)
    read_ms = write_ms = 0.0
    latencies: list[float] = []; reads: list[float] = []; writes: list[float] = []
    total_start = time.perf_counter()
    for ordinal, index in enumerate(values):
        slice_start = time.perf_counter()
        read_start = time.perf_counter()
        result = np.ascontiguousarray(slice_view(volume, args.axis, index), dtype=np.float32)
        current_read = (time.perf_counter() - read_start) * 1000
        output = args.output_dir / f"paper_{args.axis}_{args.pattern}_{ordinal}.dat"
        write_start = time.perf_counter()
        result.tofile(output)
        current_write = (time.perf_counter() - write_start) * 1000
        read_ms += current_read; write_ms += current_write
        reads.append(current_read); writes.append(current_write); latencies.append((time.perf_counter() - slice_start) * 1000)
    if args.method != "raw":
        handle.close()
    output_elements = int(np.prod([dimension for i, dimension in enumerate(shape) if i != axis_index]))
    payload = {
        "status": "SUCCESS", "format": input_format, "method": args.method, "input": str(args.input),
        "axis": args.axis, "pattern": args.pattern, "positions": values,
        "slice_latencies_ms": latencies, "slice_read_latencies_ms": reads, "slice_write_latencies_ms": writes,
        "read_time_ms": read_ms, "decode_time_ms": None, "reorder_time_ms": None, "write_time_ms": write_ms,
        "total_time_ms": (time.perf_counter() - total_start) * 1000,
        "bytes_read": None, "logical_slice_bytes": output_elements * 4 * len(values),
        "bytes_written": output_elements * 4 * len(values),
        "bytes_read_note": "OS physical bytes are not exposed by NumPy/h5py; logical bytes are reported separately.",
    }
    args.metrics_json.parent.mkdir(parents=True, exist_ok=True)
    args.metrics_json.write_text(json.dumps(payload, indent=2) + "\n")


if __name__ == "__main__":
    main()
