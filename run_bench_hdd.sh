#!/bin/bash
# ERWT3D HDD 测试

input = /mnt/f/zhoujiawang/CUP/cup_3d_small.erwt3d
output = /tmp/out_hdd

erwt3d begin mytest
    erwt3d bench_hdd input=$input output=$output
erwt3d end
