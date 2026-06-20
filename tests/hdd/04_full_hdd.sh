#!/bin/bash
# 完整 HDD 测试 (100 + 10)

input = /mnt/f/zhoujiawang/CUP/cup_3d_small.erwt3d
output_dir = /tmp/test_hdd

erwt3d begin full_hdd
    erwt3d bench_hdd input=$input output=$output_dir/full random=100 continuous=10
erwt3d end
