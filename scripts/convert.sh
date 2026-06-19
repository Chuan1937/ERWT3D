#!/bin/bash
# RAW -> ERWT3D 转换

# ========== 配置 ==========
input=/mnt/f/zhoujiawang/CUP/cup_3d_small.dat
output=/mnt/f/zhoujiawang/CUP/cup_3d_small.erwt3d
nx=801
ny=2405
nz=2501
threads=8
memory_limit_mb=4096

# ========== 运行 ==========
# 先写到 SSD，再复制（HDD 转换太慢）
tmpfile=/tmp/erwt3d_convert_$$.erwt3d

./build/erwt3d_convert \
  -i $input \
  -o $tmpfile \
  --nx $nx --ny $ny --nz $nz \
  -t $threads \
  -m $memory_limit_mb

cp $tmpfile $output
rm -f $tmpfile
echo "Done: $output"
