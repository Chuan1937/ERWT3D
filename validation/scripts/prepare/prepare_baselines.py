#!/usr/bin/env python3
"""Prepare storage baselines; LZ4/ZFP are not random-access containers."""
from __future__ import annotations
import argparse, json, shutil, time
from pathlib import Path
import numpy as np

def accuracy(raw: np.ndarray, decoded: np.ndarray) -> dict:
    delta = raw.astype(np.float64) - decoded.astype(np.float64)
    relative = np.abs(delta[np.abs(raw) > 1e-6] / raw[np.abs(raw) > 1e-6])
    return {"rmse": float(np.sqrt(np.mean(delta * delta))), "nrmse": float(np.sqrt(np.sum(delta * delta) / np.sum(raw.astype(np.float64) ** 2))) if np.any(raw) else 0.0, "max_actual_relative_error": float(relative.max()) if relative.size else 0.0}

def record(method, output, raw, encode_ms, decode_ms, decoded, dataset, parameter=None) -> dict:
    return {"status": "SUCCESS", "experiment": "storage", "dataset": dataset, "method": method,
            "parameter": parameter or {}, "raw_size_bytes": raw.nbytes, "compressed_size_bytes": output.stat().st_size,
            "compression_ratio": raw.nbytes / output.stat().st_size, "encode_time_ms": encode_ms,
            "decode_time_ms": decode_ms, **accuracy(raw, decoded)}

def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--raw", type=Path, required=True); p.add_argument("--shape", nargs=3, type=int, required=True)
    p.add_argument("--output-dir", type=Path, required=True); p.add_argument("--metrics-dir", type=Path)
    p.add_argument("--dataset", default=""); p.add_argument("--methods", nargs="+", default=["raw", "lz4", "hdf5_raw", "hdf5_gzip", "zfp"])
    args = p.parse_args(); raw = np.memmap(args.raw, dtype=np.float32, mode="r", shape=tuple(args.shape)); args.output_dir.mkdir(parents=True, exist_ok=True)
    results = []
    for method in args.methods:
        try:
            start = time.perf_counter()
            if method == "raw":
                output = args.output_dir / "baseline.raw"; shutil.copyfile(args.raw, output); encode_ms = (time.perf_counter() - start) * 1000
                start = time.perf_counter(); decoded = np.array(raw); decode_ms = (time.perf_counter() - start) * 1000
                results.append(record(method, output, raw, encode_ms, decode_ms, decoded, args.dataset))
            elif method == "lz4":
                import lz4.frame
                output = args.output_dir / "baseline.lz4"; output.write_bytes(lz4.frame.compress(raw.tobytes())); encode_ms = (time.perf_counter() - start) * 1000
                start = time.perf_counter(); decoded = np.frombuffer(lz4.frame.decompress(output.read_bytes()), dtype=np.float32).reshape(raw.shape); decode_ms = (time.perf_counter() - start) * 1000
                results.append(record(method, output, raw, encode_ms, decode_ms, decoded, args.dataset))
            elif method in {"hdf5_raw", "hdf5_gzip"}:
                import h5py
                output = args.output_dir / f"baseline_{method}.h5"; kwargs = {"chunks": tuple(min(64, x) for x in raw.shape)}
                if method == "hdf5_gzip": kwargs.update(compression="gzip", compression_opts=4)
                with h5py.File(output, "w") as f: f.create_dataset("volume", data=raw, **kwargs)
                encode_ms = (time.perf_counter() - start) * 1000; start = time.perf_counter()
                with h5py.File(output, "r") as f: decoded = f["volume"][...]
                decode_ms = (time.perf_counter() - start) * 1000
                results.append(record(method, output, raw, encode_ms, decode_ms, decoded, args.dataset))
            elif method == "zfp":
                import zfpy
                for tolerance in (1e-4, 5e-4, 1e-3, 5e-3):
                    start = time.perf_counter(); output = args.output_dir / f"baseline_zfp_{tolerance:g}.zfp"
                    payload = zfpy.compress_numpy(np.array(raw), tolerance=tolerance); output.write_bytes(payload); encode_ms = (time.perf_counter() - start) * 1000
                    start = time.perf_counter(); decoded = zfpy.decompress_numpy(payload); decode_ms = (time.perf_counter() - start) * 1000
                    results.append(record(method, output, raw, encode_ms, decode_ms, decoded, args.dataset, {"tolerance": tolerance}))
            else: raise ValueError(f"unknown method {method}")
        except ImportError as error:
            results.append({"status": "SKIPPED", "experiment": "storage", "dataset": args.dataset, "method": method, "reason": f"missing optional dependency: {error.name}"})
    (args.output_dir / "baseline_results.json").write_text(json.dumps(results, indent=2) + "\n")
    if args.metrics_dir:
        args.metrics_dir.mkdir(parents=True, exist_ok=True)
        for ordinal, result in enumerate(results):
            (args.metrics_dir / f"{result['method']}_{ordinal}.json").write_text(json.dumps(result, indent=2) + "\n")

if __name__ == "__main__": main()
