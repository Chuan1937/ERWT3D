#!/usr/bin/env python3
"""Generate figures for the ERWT3D competition report."""

import csv
import os
import sys

def read_csv(path):
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return list(csv.DictReader(f))

def get_avg(rows, axis, mode):
    for r in rows:
        if r['axis'] == axis and r['mode'] == mode:
            return float(r['avg_time_ms'])
    return 0

def plot_all(results_dir, output_dir):
    """Generate all 5 figures."""
    os.makedirs(output_dir, exist_ok=True)

    try:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
        import numpy as np
    except ImportError:
        print("matplotlib not available")
        return False

    erwt3d = read_csv(f'{results_dir}/syn256_erwt3d_t8_cache512.csv')
    raw    = read_csv(f'{results_dir}/syn256_raw_baseline.csv')
    cache512 = read_csv(f'{results_dir}/syn256_erwt3d_t8_cache512.csv')
    cache0  = read_csv(f'{results_dir}/syn256_erwt3d_t8_cache0.csv')
    summary = read_csv(f'{results_dir}/summary_table.csv')
    tscale  = read_csv(f'{results_dir}/thread_scaling.csv')

    axes = ['x', 'y', 'z']
    width = 0.35
    xpos = np.arange(len(axes))

    # --- Figure 1: Random slice comparison ---
    if erwt3d and raw:
        fig, ax = plt.subplots(figsize=(8, 5))
        ev = [get_avg(erwt3d, a, 'random') for a in axes]
        rv = [get_avg(raw, a, 'random') for a in axes]
        ax.bar(xpos - width/2, ev, width, label='ERWT3D', color='steelblue')
        ax.bar(xpos + width/2, rv, width, label='Raw row-major', color='lightcoral')
        ax.set_ylabel('Time (ms)')
        ax.set_title('Random Slice Read Performance (256x256x256)')
        ax.set_xticks(xpos); ax.set_xticklabels([a.upper() for a in axes])
        ax.legend()
        plt.tight_layout(); plt.savefig(f'{output_dir}/axis_random_comparison.png', dpi=100); plt.close()
        print(f"  Saved {output_dir}/axis_random_comparison.png")

    # --- Figure 2: Continuous slice comparison ---
    if erwt3d and raw:
        fig, ax = plt.subplots(figsize=(8, 5))
        ev = [get_avg(erwt3d, a, 'continuous') for a in axes]
        rv = [get_avg(raw, a, 'continuous') for a in axes]
        ax.bar(xpos - width/2, ev, width, label='ERWT3D', color='steelblue')
        ax.bar(xpos + width/2, rv, width, label='Raw row-major', color='lightcoral')
        ax.set_ylabel('Time (ms)')
        ax.set_title('Continuous Slice Read Performance (256x256x256)')
        ax.set_xticks(xpos); ax.set_xticklabels([a.upper() for a in axes])
        ax.legend()
        plt.tight_layout(); plt.savefig(f'{output_dir}/axis_continuous_comparison.png', dpi=100); plt.close()
        print(f"  Saved {output_dir}/axis_continuous_comparison.png")

    # --- Figure 3: Thread scaling ---
    if tscale:
        fig, ax = plt.subplots(figsize=(6, 4))
        ts = [int(r.get('threads', 0)) for r in tscale]
        vs = [float(r.get('T_total_ms', 0)) for r in tscale]
        ax.plot(ts, vs, 'o-', color='steelblue', linewidth=2, markersize=8)
        ax.set_xlabel('Threads')
        ax.set_ylabel('Total time (ms)')
        ax.set_title('Thread Scaling (256x256x256, 20 random slices)')
        ax.grid(True, alpha=0.3)
        plt.tight_layout(); plt.savefig(f'{output_dir}/thread_scaling.png', dpi=100); plt.close()
        print(f"  Saved {output_dir}/thread_scaling.png")

    # --- Figure 4: Cache effect ---
    if cache512 and cache0:
        fig, ax = plt.subplots(figsize=(7, 4))
        c512v = [get_avg(cache512, a, 'random') for a in axes]
        c0v  = [get_avg(cache0, a, 'random') for a in axes]
        ax.bar(xpos - width/2, c512v, width, label='Cache 512MB', color='steelblue')
        ax.bar(xpos + width/2, c0v, width, label='Cache 0', color='lightcoral')
        ax.set_ylabel('Time (ms)')
        ax.set_title('Cache Effect: Random Slice (256x256x256, t8)')
        ax.set_xticks(xpos); ax.set_xticklabels([a.upper() for a in axes])
        ax.legend()
        plt.tight_layout(); plt.savefig(f'{output_dir}/cache_effect.png', dpi=100); plt.close()
        print(f"  Saved {output_dir}/cache_effect.png")

    # --- Figure 5: Storage ratio ---
    fig, ax = plt.subplots(figsize=(5, 4))
    labels = ['256^3 (aligned)', '801x2405x2501 (CUP)']
    ratios = [1.000, 1.075]
    bars = ax.bar(labels, ratios, color=['steelblue', 'lightcoral'])
    ax.axhline(y=1.5, color='red', linestyle='--', label='1.5x limit')
    ax.axhline(y=1.0, color='green', linestyle=':', label='1.0x (raw)')
    ax.set_ylabel('Storage ratio')
    ax.set_title('ERWT3D Storage Ratio')
    ax.legend()
    for bar, val in zip(bars, ratios):
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.01, f'{val:.3f}x', ha='center')
    plt.tight_layout(); plt.savefig(f'{output_dir}/storage_ratio.png', dpi=100); plt.close()
    print(f"  Saved {output_dir}/storage_ratio.png")

    return True


if __name__ == '__main__':
    results_dir = sys.argv[1] if len(sys.argv) > 1 else 'docs/results'
    output_dir = sys.argv[2] if len(sys.argv) > 2 else 'docs/figures'
    plot_all(results_dir, output_dir)
    print("Done.")