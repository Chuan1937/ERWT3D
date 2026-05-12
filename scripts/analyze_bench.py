#!/usr/bin/env python3
"""Analyze ERWT3D benchmark CSV files and generate summary tables."""

import csv
import sys
import os
from collections import defaultdict

def read_bench_csv(path):
    """Read a bench_result.csv and return dict of {axis_mode: avg_time_ms}."""
    result = {}
    with open(path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            key = f"{row['axis']}_{row['mode']}"
            result[key] = float(row['avg_time_ms'])
            key_total = f"{row['axis']}_{row['mode']}_total"
            result[key_total] = float(row['total_time_ms'])
    return result

def balance_ratio(vals):
    """Return max/min ratio."""
    return max(vals) / min(vals) if min(vals) > 0 else float('inf')

def analyze(bench_dirs, output_prefix):
    """Analyze benchmark results."""
    configs = {}
    
    for name, paths in bench_dirs.items():
        if not os.path.exists(paths[0]):
            print(f"Warning: {paths[0]} not found, skipping {name}")
            continue
        configs[name] = read_bench_csv(paths[0])
    
    # Summary table
    with open(f"{output_prefix}/summary_table.csv", "w") as f:
        f.write("config,T_x_random,T_y_random,T_z_random,T_random_avg,T_x_cont,T_y_cont,T_z_cont,T_cont_avg,T_total,random_balance,cont_balance\n")
        for name, data in configs.items():
            xr, yr, zr = data.get('x_random', 0), data.get('y_random', 0), data.get('z_random', 0)
            xc, yc, zc = data.get('x_continuous', 0), data.get('y_continuous', 0), data.get('z_continuous', 0)
            ravg = (xr + yr + zr) / 3
            cavg = (xc + yc + zc) / 3
            ttotal = (ravg + cavg) / 2
            rbal = balance_ratio([xr, yr, zr])
            cbal = balance_ratio([xc, yc, zc])
            f.write(f"{name},{xr:.2f},{yr:.2f},{zr:.2f},{ravg:.2f},{xc:.2f},{yc:.2f},{zc:.2f},{cavg:.2f},{ttotal:.2f},{rbal:.2f},{cbal:.2f}\n")
    
    # Thread scaling table
    t1 = configs.get('erwt3d_t1', {}).get('x_random_total', 0) + configs.get('erwt3d_t1', {}).get('y_random_total', 0) + configs.get('erwt3d_t1', {}).get('z_random_total', 0)
    with open(f"{output_prefix}/thread_scaling.csv", "w") as f:
        f.write("threads,total_time_ms,speedup\n")
        for t in [1, 2, 4, 8]:
            key = f'erwt3d_t{t}'
            if key in configs:
                total = configs[key].get('x_random_total', 0) + configs[key].get('y_random_total', 0) + configs[key].get('z_random_total', 0)
                speedup = t1 / total if t1 > 0 and total > 0 else 0
                f.write(f"{t},{total:.2f},{speedup:.2f}\n")
    
    print(f"Analysis complete. Output: {output_prefix}/")

if __name__ == '__main__':
    output_dir = sys.argv[1] if len(sys.argv) > 1 else "docs/results"
    
    bench_dirs = {
        'erwt3d_t8_cache512': (f'{output_dir}/syn256_erwt3d_t8_cache512.csv',),
        'erwt3d_t8_cache0': (f'{output_dir}/syn256_erwt3d_t8_cache0.csv',),
        'erwt3d_t1': (f'{output_dir}/syn256_erwt3d_t1_cache0.csv',),
        'raw_baseline': (f'{output_dir}/syn256_raw_baseline.csv',),
    }
    
    analyze(bench_dirs, output_dir)