#!/usr/bin/env python3
"""Invoke an external forward solver and compare original/reconstructed gathers."""
from __future__ import annotations
import argparse, json, subprocess
from pathlib import Path
import numpy as np

def metrics(a: np.ndarray, b: np.ndarray) -> dict[str, float]:
    delta = a.astype(np.float64) - b.astype(np.float64)
    return {"rmse": float(np.sqrt(np.mean(delta * delta))), "nrmse": float(np.sqrt(np.sum(delta * delta) / np.sum(a.astype(np.float64) ** 2))) if np.any(a) else 0.0}

def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--original-model", type=Path, required=True); p.add_argument("--reconstructed-model", type=Path, required=True)
    p.add_argument("--original-gather", type=Path, required=True); p.add_argument("--reconstructed-gather", type=Path, required=True)
    p.add_argument("--nrec", type=int, required=True, help="Number of receiver traces")
    p.add_argument("--nt", type=int, required=True, help="Samples per receiver trace")
    p.add_argument("--solver-command", help="Optional shell template: {model}, {gather}, {label}"); p.add_argument("--output", type=Path, required=True)
    args = p.parse_args()
    for label, model, gather in (("original", args.original_model, args.original_gather), ("reconstructed", args.reconstructed_model, args.reconstructed_gather)):
        if not gather.exists() and args.solver_command: subprocess.run(args.solver_command.format(model=model, gather=gather, label=label), shell=True, check=True)
        if not model.is_file() or not gather.is_file(): raise SystemExit(f"missing {label} model or gather")
    original_model, reconstructed_model = np.memmap(args.original_model, dtype=np.float32, mode="r"), np.memmap(args.reconstructed_model, dtype=np.float32, mode="r")
    original_gather, reconstructed_gather = np.memmap(args.original_gather, dtype=np.float32, mode="r"), np.memmap(args.reconstructed_gather, dtype=np.float32, mode="r")
    if original_model.size != reconstructed_model.size or original_gather.size != reconstructed_gather.size: raise SystemExit("original and reconstructed arrays must have equal element counts")
    if args.nrec < 1 or args.nt < 1 or args.nrec * args.nt != original_gather.size: raise SystemExit("--nrec * --nt must equal the gather element count")
    original_traces = original_gather.reshape(args.nrec, args.nt).astype(np.float64)
    reconstructed_traces = reconstructed_gather.reshape(args.nrec, args.nt).astype(np.float64)
    numerator = np.linalg.norm(reconstructed_traces - original_traces, axis=1)
    denominator = np.linalg.norm(original_traces, axis=1)
    receiver_nrmse = np.divide(numerator, denominator, out=np.zeros_like(numerator), where=denominator != 0)
    result = {"status": "SUCCESS", "model": metrics(original_model, reconstructed_model), "waveform": metrics(original_gather, reconstructed_gather), "gather_shape": [args.nrec, args.nt], "per_receiver_nrmse": {"mean": float(receiver_nrmse.mean()), "median": float(np.median(receiver_nrmse)), "p95": float(np.percentile(receiver_nrmse, 95)), "max": float(receiver_nrmse.max())}}
    args.output.parent.mkdir(parents=True, exist_ok=True); args.output.write_text(json.dumps(result, indent=2) + "\n")

if __name__ == "__main__": main()
