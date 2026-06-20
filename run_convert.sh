#!/bin/bash
# ERWT3D 转换

input = /mnt/f/zhoujiawang/CUP/cup_3d_small.dat
output = /mnt/f/zhoujiawang/CUP/cup_3d_small.erwt3d

erwt3d begin convert
    erwt3d convert input=$input output=$output nx=801 ny=2405 nz=2501
erwt3d end
