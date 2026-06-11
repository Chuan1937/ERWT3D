#pragma once

#include "sb_plan.hpp"

namespace erwt3d {

// ========== Panel Optimization ==========
//
// Panel: 预存某个轴的切片平面数据，避免读取整个superblock
// 适用于: 频繁访问单轴切片的场景

// tryReadSliceXPanels: 尝试用X-Panel数据读取切片
// 返回true表示成功(命中panel)，false表示panel不可用
bool tryReadSliceXPanels(int fd, const ERWT3DHeader& hdr, uint64_t x,
                          float* output, IOProfile* profile);

// tryReadSliceXPanelsParallel: 多线程版本
bool tryReadSliceXPanelsParallel(int fd, const ERWT3DHeader& hdr, uint64_t x,
                                  float* output, int numThreads, IOProfile* profile);

// TODO: Y-Panel and Z-Panel support can be added here

} // namespace erwt3d
