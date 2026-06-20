#!/bin/bash
# 多切片顺序读取

input = /mnt/f/zhoujiawang/CUP/cup_3d_small.erwt3d
output_dir = /tmp/test_hdd

erwt3d begin multi_slice
    erwt3d slice input=$input axis=Z index=1250 output=$output_dir/z_0.raw
    erwt3d slice input=$input axis=Z index=1251 output=$output_dir/z_1.raw
    erwt3d slice input=$input axis=Z index=1252 output=$output_dir/z_2.raw
    erwt3d slice input=$input axis=Z index=1253 output=$output_dir/z_3.raw
    erwt3d slice input=$input axis=Z index=1254 output=$output_dir/z_4.raw
    erwt3d slice input=$input axis=Z index=1255 output=$output_dir/z_5.raw
    erwt3d slice input=$input axis=Z index=1256 output=$output_dir/z_6.raw
    erwt3d slice input=$input axis=Z index=1257 output=$output_dir/z_7.raw
    erwt3d slice input=$input axis=Z index=1258 output=$output_dir/z_8.raw
    erwt3d slice input=$input axis=Z index=1259 output=$output_dir/z_9.raw
erwt3d end
