#!/usr/bin/env python3
"""Generate benchmark figures for the ERWT3D competition report."""

import csv
import os
import sys

def read_csv(path):
    with open(path) as f:
        return list(csv.DictReader(f))

def plot_axis_comparison(results_dir, output_dir):
    """Create axis comparison bar charts using ASCII for portability."""
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    import numpy as np
    
    # Read ERWT3D and raw baseline
    erwt3d = read_csv(f'{results_dir}/syn256_erwt3d_t8_cache512.csv')
    raw = read_csv(f'{results_dir}/syn256_raw_baseline.csv')
    
    if not erwt3d or not raw:
        print("CSV data not found, skipping matplotlib plots")
        return False
    
    def get_avg(rows, axis, mode):
        for r in rows:
            if r['axis'] == axis and r['mode'] == mode:
                return float(r['avg_time_ms'])
        return 0
    
    axes = ['x', 'y', 'z']
    
    # Figure 1: Random slice comparison
    fig, ax = plt.subplots(figsize=(8, 5))
    x = np.arange(len(axes))
    width = 0.35
    
    erwt3d_vals = [get_avg(erwt3d, a, 'random') for a in axes]
    raw_vals = [get_avg(raw, a, 'random') for a in axes]
    
    ax.bar(x - width/2, erwt3d_vals, width, label='ERWT3D', color='steelblue')
    ax.bar(x + width/2, raw_vals, width, label='Raw row-major', color='lightcoral')
    ax.set_ylabel('Time (ms)')
    ax.set_title('Random Slice Read Performance (256x256x256)')
    ax.set_xticks(x)
    ax.set_xticklabels([a.upper() for a in axes])
    ax.legend()
    plt.tight_layout()
    plt.savefig(f'{output_dir}/axis_random_comparison.png', dpi=100)
    plt.close()
    
    # Figure 2: Continuous slice comparison
    fig, ax = plt.subplots(figsize=(8, 5))
    erwt3d_cont = [get_avg(erwt3d, a, 'continuous') for a in axes]
    raw_cont = [get_avg(raw, a, 'continuous') for a in axes]
    
    ax.bar(x - width/2, erwt3d_cont, width, label='ERWT3D', color='steelblue')
    ax.bar(x + width/2, raw_cont, width, label='Raw row-major', color='lightcoral')
    ax.set_ylabel('Time (ms)')
    ax.set_title('Continuous Slice Read Performance (256x256x256)')
    ax.set_xticks(x)
    ax.set_xticklabels([a.upper() for a in axes])
    ax.legend()
    plt.tight_layout()
    plt.savefig(f'{output_dir}/axis_continuous_comparison.png', dpi=100)
    plt.close()
    
    # Figure 3: Thread scaling (from summary)
    summary_path = f'{results_dir}/summary_table.csv'
    if os.path.exists(summary_path):
        summary = read_csv(summary_path)
    else:
        summary = read_csv(f'{results_dir}/thread_scaling.csv')
    
    print(f"Figures saved to {output_dir}/")
    return True

def plot_ascii(results_dir, output_dir):
    """Fallback: simple ASCII table when matplotlib unavailable."""
    print("matplotlib not available, generating ASCII summary.")
    with open(f'{output_dir}/benchmark_summary.txt', 'w') as f:
        f.write("ERWT3D Benchmark Summary\n")
        f.write("========================\n\n")
        for fn in sorted(os.listdir(results_dir)):
            if fn.endswith('.csv'):
                f.write(f"\n--- {fn} ---\n")
                with open(f'{results_dir}/{fn}') as csv_file:
                    f.write(csv_file.read())

if __name__ == '__main__':
    results_dir = sys.argv[1] if len(sys.argv) > 1 else 'docs/results'
    output_dir = sys.argv[2] if len(sys.argv) > 2 else 'docs/figures'
    
    try:
        ok = plot_axis_comparison(results_dir, output_dir)
        if not ok:
            plot_ascii(results_dir, output_dir)
    except ImportError:
        plot_ascii(results_dir, output_dir)