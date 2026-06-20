#!/bin/bash
# ERWT3D SSD 测试

input = /mnt/f/zhoujiawang/CUP/cup_3d_small.erwt3d
output = /tmp/out_ssd

erwt3d begin mytest
    erwt3d bench_ssd input=$input output=$output
erwt3d end
