#!/bin/bash
# ERWT3D SSD 性能测试

# ========== 配置 ==========
input=/mnt/f/zhoujiawang/CUP/cup_3d_small.erwt3d
output=/tmp/out_ssd
random_count=100
continuous_count=10
threads=8
memory_limit_mb=8192

# ========== 运行 ==========
./build/erwt3d_bench_contest \
  -i $input \
  -o $output \
  --random-count $random_count \
  --continuous-count $continuous_count \
  -t $threads \
  -m $memory_limit_mb \
  --io-backend sb \
  --sb-parallel-mode parallel-read \
  --sb-task-order file-offset
