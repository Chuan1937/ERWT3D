#!/bin/bash
# ERWT3D SSD 测试

erwt3d begin ssd_test
    erwt3d bench_ssd input=/mnt/f/zhoujiawang/CUP/cup_3d_small.erwt3d output=/tmp/out_ssd
erwt3d end
