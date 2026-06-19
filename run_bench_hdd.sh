#!/bin/bash
# ERWT3D 测试脚本

erwt3d begin mytest
    erwt3d bench_hdd input=/mnt/f/zhoujiawang/CUP/cup_3d_small.erwt3d output=/tmp/out_hdd
erwt3d end
