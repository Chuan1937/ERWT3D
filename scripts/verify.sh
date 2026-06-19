#!/bin/bash
# 正确性验证

# ========== 配置 ==========
raw=/mnt/f/zhoujiawang/CUP/cup_3d_small.dat
erwt3d=/mnt/f/zhoujiawang/CUP/cup_3d_small.erwt3d
nx=801
ny=2405
nz=2501
samples=100000

# ========== 运行 ==========
./build/erwt3d_verify \
  --raw $raw \
  --erwt3d $erwt3d \
  --nx $nx --ny $ny --nz $nz \
  --samples $samples
