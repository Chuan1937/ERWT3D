#!/usr/bin/env python3
"""Prepare storage baselines and record measured reconstruction accuracy.

This is intentionally a compression/storage tool. Standalone LZ4 and ZFP are
not used as purportedly fair multi-axis random-access containers.
"""
from __future__ import annotations
import argparse, json, shutil, time
from pathlib import Path
import numpy as np

def stats(raw: np.ndarray, decoded: np.ndarray) -> dict:
    diff = raw.astype(np.float64) - decoded.astype(np.float64)
    rel = np.abs(diff[np.abs(raw) > 1e-6] / raw[np.abs(raw) > 1e-6])
    return {"rmse": float(np.sqrt(np.mean(diff * diff))), "nrmse": float(np.sqrt(np.sum(diff * diff) / np.sum(raw.astype(np.float64) ** 2))) if np.any(raw) else 0.0, "max_actual_relative_error": float(rel.max()) if rel.size else 0.0}

def main() -> None:
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--raw",type=Path,required=True); p.add_argument("--shape",nargs=3,type=int,required=True); p.add_argument("--output-dir",type=Path,required=True)
    p.add_argument("--methods",nargs="+",default=["raw","lz4","hdf5_raw","hdf5_gzip","zfp"]); args=p.parse_args()
    raw=np.memmap(args.raw,dtype=np.float32,mode="r",shape=tuple(args.shape)); args.output_dir.mkdir(parents=True,exist_ok=True); results=[]
    for method in args.methods:
        try:
            start=time.perf_counter(); decoded=None
            if method=="raw":
                output=args.output_dir/"baseline.raw"; shutil.copyfile(args.raw,output); decoded=np.array(raw)
            elif method=="lz4":
                import lz4.frame
                output=args.output_dir/"baseline.lz4"; output.write_bytes(lz4.frame.compress(raw.tobytes())); decoded=np.frombuffer(lz4.frame.decompress(output.read_bytes()),dtype=np.float32).reshape(raw.shape)
            elif method in {"hdf5_raw","hdf5_gzip"}:
                import h5py
                output=args.output_dir/f"baseline_{method}.h5"; kwargs={"chunks":(64,64,64)}
                if method=="hdf5_gzip": kwargs.update(compression="gzip",compression_opts=4)
                with h5py.File(output,"w") as f: f.create_dataset("volume",data=raw,**kwargs)
                with h5py.File(output,"r") as f: decoded=f["volume"][...]
            elif method=="zfp":
                import zfpy
                for tolerance in (1e-4, 5e-4, 1e-3, 5e-3):
                    output=args.output_dir/f"baseline_zfp_{tolerance:g}.zfp"; payload=zfpy.compress_numpy(np.array(raw),tolerance=tolerance); output.write_bytes(payload); decoded=zfpy.decompress_numpy(payload)
                    results.append({"status":"SUCCESS","method":"zfp","parameter":{"tolerance":tolerance},"compressed_size_bytes":output.stat().st_size,"encode_decode_time_ms":(time.perf_counter()-start)*1000,**stats(raw,decoded)})
                continue
            else: raise ValueError(f"unknown method {method}")
            results.append({"status":"SUCCESS","method":method,"compressed_size_bytes":output.stat().st_size,"encode_decode_time_ms":(time.perf_counter()-start)*1000,**stats(raw,decoded)})
        except ImportError as error:
            results.append({"status":"SKIPPED","method":method,"reason":f"missing optional dependency: {error.name}"})
    (args.output_dir/"baseline_results.json").write_text(json.dumps(results,indent=2)+"\n")

if __name__=="__main__": main()
