#!/usr/bin/env python3
"""Analyze ERWT3D bench_result.csv files and generate summary tables."""

import csv
import sys
import os

def read_bench_csv(path):
    """Read a bench_result.csv, return {axis_mode: avg_time_ms}."""
    result = {}
    if not os.path.exists(path):
        return result
    with open(path) as f:
        for row in csv.DictReader(f):
            key = f"{row['axis']}_{row['mode']}"
            result[key] = float(row['avg_time_ms'])
            result[f"{key}_total"] = float(row['total_time_ms'])
    return result

def balance_ratio(vals):
    return max(vals) / min(vals) if min(vals) > 0 else float('inf')

def calc_t_total(data):
    """Compute T_total = (random_avg + cont_avg) / 2 (matches docs/benchmark.md)."""
    xr = data.get('x_random', 0); yr = data.get('y_random', 0); zr = data.get('z_random', 0)
    xc = data.get('x_continuous', 0); yc = data.get('y_continuous', 0); zc = data.get('z_continuous', 0)
    random_avg = (xr + yr + zr) / 3
    cont_avg = (xc + yc + zc) / 3
    return (random_avg + cont_avg) / 2

def analyze(files, output_dir):
    configs = {}
    for name, path in files.items():
        data = read_bench_csv(path)
        if data:
            configs[name] = data
            print(f"  Loaded {name} from {path}")
        else:
            print(f"  Skipped {name} (not found)")

    if not configs:
        print("No CSV files found.")
        return

    # Summary table
    with open(f"{output_dir}/summary_table.csv", "w") as f:
        f.write("config,T_x_random,T_y_random,T_z_random,T_random_avg,"
                "T_x_cont,T_y_cont,T_z_cont,T_cont_avg,T_total,"
                "random_balance,cont_balance\n")
        for name, data in configs.items():
            xr = data.get('x_random', 0); yr = data.get('y_random', 0); zr = data.get('z_random', 0)
            xc = data.get('x_continuous', 0); yc = data.get('y_continuous', 0); zc = data.get('z_continuous', 0)
            ravg = (xr + yr + zr) / 3; cavg = (xc + yc + zc) / 3
            ttotal = (ravg + cavg) / 2
            f.write(f"{name},{xr:.2f},{yr:.2f},{zr:.2f},{ravg:.2f},"
                    f"{xc:.2f},{yc:.2f},{zc:.2f},{cavg:.2f},{ttotal:.2f},"
                    f"{balance_ratio([xr,yr,zr]):.2f},{balance_ratio([xc,yc,zc]):.2f}\n")

    # Thread scaling: uses T_total formula (same as docs)
    with open(f"{output_dir}/thread_scaling.csv", "w") as f:
        f.write("threads,T_total_ms,speedup\n")
        base = calc_t_total(configs.get('erwt3d_t1', {})) if 'erwt3d_t1' in configs else 1
        for t in [1, 2, 4, 8]:
            name = f'erwt3d_t{t}'
            if name in configs:
                total = calc_t_total(configs[name])
                speedup = base / total if base > 0 and total > 0 else 0
                f.write(f"{t},{total:.2f},{speedup:.2f}\n")

    # Cache comparison
    with open(f"{output_dir}/cache_comparison.csv", "w") as f:
        f.write("axis,cache512_ms,cache0_ms,diff_pct\n")
        c512 = configs.get('erwt3d_t8_cache512', {})
        c0   = configs.get('erwt3d_t8_cache0', {})
        for axis in ['x', 'y', 'z']:
            v512 = c512.get(f'{axis}_random', 0)
            v0   = c0.get(f'{axis}_random', 0)
            pct  = ((v512 - v0) / v0 * 100) if v0 > 0 else 0
            f.write(f"{axis},{v512:.2f},{v0:.2f},{pct:+.1f}\n")

    print(f"\nOutput written to {output_dir}/")


if __name__ == '__main__':
    output_dir = sys.argv[1] if len(sys.argv) > 1 else "docs/results"

    files = {
        'erwt3d_t1':          f'{output_dir}/syn256_erwt3d_t1_cache0.csv',
        'erwt3d_t8_cache512': f'{output_dir}/syn256_erwt3d_t8_cache512.csv',
        'erwt3d_t8_cache0':   f'{output_dir}/syn256_erwt3d_t8_cache0.csv',
        'raw_baseline':       f'{output_dir}/syn256_raw_baseline.csv',
    }
    # Add t2/t4/t8 if available
    for t in [2, 4]:
        path = f'{output_dir}/syn256_erwt3d_t{t}_cache0.csv'
        if os.path.exists(path):
            files[f'erwt3d_t{t}'] = path
    path = f'{output_dir}/syn256_erwt3d_t8_cache0.csv'
    if os.path.exists(path) and 'erwt3d_t8' not in files:
        files['erwt3d_t8'] = path

    analyze(files, output_dir)