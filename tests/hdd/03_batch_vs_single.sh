#!/bin/bash
# Batch vs 逐切片对比

input = /mnt/f/zhoujiawang/CUP/cup_3d_small.erwt3d
output_dir = /tmp/test_hdd

erwt3d begin batch_vs_single
    erwt3d bench_hdd input=$input output=$output_dir/single random=10 continuous=5
erwt3d end
