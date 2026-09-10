#!/usr/bin/env python3
"""Create HDF5 chunked-raw and gzip baselines without loading a full volume."""
from __future__ import annotations
import argparse
from pathlib import Path
import numpy as np

def write(raw, output: Path, compression: str | None) -> None:
    import h5py
    output.parent.mkdir(parents=True, exist_ok=True)
    with h5py.File(output, "w") as h5:
        kwargs = {"shape": raw.shape, "dtype": np.float32, "chunks": tuple(min(64, x) for x in raw.shape)}
        if compression: kwargs.update(compression="gzip", compression_opts=4)
        volume = h5.create_dataset("volume", **kwargs)
        for start in range(0, raw.shape[0], 64): volume[start:start + 64] = raw[start:start + 64]

def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--raw", type=Path, required=True); p.add_argument("--shape", nargs=3, type=int, required=True); p.add_argument("--output-dir", type=Path, required=True)
    args = p.parse_args(); raw = np.memmap(args.raw, dtype=np.float32, mode="r", shape=tuple(args.shape), order="C")
    write(raw, args.output_dir / "hdf5_raw.h5", None); write(raw, args.output_dir / "hdf5_gzip.h5", "gzip")

if __name__ == "__main__": main()
