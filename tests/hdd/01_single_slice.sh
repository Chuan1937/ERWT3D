#!/bin/bash
# 单切片读取延迟

input = /mnt/f/zhoujiawang/CUP/cup_3d_small.erwt3d
output_dir = /tmp/test_hdd

erwt3d begin single_slice
    erwt3d slice input=$input axis=X index=400 output=$output_dir/x.raw
    erwt3d slice input=$input axis=Y index=1200 output=$output_dir/y.raw
    erwt3d slice input=$input axis=Z index=1250 output=$output_dir/z.raw
erwt3d end
