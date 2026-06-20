#!/bin/bash
# 正确性验证

raw = /mnt/f/zhoujiawang/CUP/cup_3d_small.dat
erwt3d_file = /mnt/f/zhoujiawang/CUP/cup_3d_small.erwt3d

erwt3d begin verify
    erwt3d verify raw=$raw erwt3d=$erwt3d_file nx=801 ny=2405 nz=2501
erwt3d end
