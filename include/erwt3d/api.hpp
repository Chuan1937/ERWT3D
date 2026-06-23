#pragma once

#include "reader.hpp"
#include "writer.hpp"
#include <string>
#include <vector>
#include <cstdint>

namespace erwt3d {

// ========== 转换参数 ==========
struct ConvertConfig {
    std::string input;
    std::string output;
    uint64_t nx, ny, nz;
    uint32_t superSize = 64;
    uint32_t leafSize = 4;
    uint32_t panelAxis = 0;
    uint32_t panelStride = 0;
    int numThreads = 8;
    size_t memoryLimitMB = 4096;
};

// ========== 测试参数 ==========
struct BenchConfig {
    std::string input;
    std::string outputDir;
    int randomCount = 100;
    int continuousCount = 10;
    int numThreads = 1;
    size_t memoryLimitMB = 4096;
    size_t cacheMB = 0;
    IOBackend ioBackend = IOBackend::Superblock;
    SBReadMode readMode = SBReadMode::HDDReadWindow;
    SBTaskOrder taskOrder = SBTaskOrder::FileOffset;
    uint64_t readWindowBytes = 33554432;
    uint64_t maxGapBytes = 1048576;
    bool batchPlanner = true;
    bool hddMode = true;
    uint32_t seed = 20260511;
};

// ========== Contest 结果 ==========
struct ContestResult {
    double T_x_random_ms;
    double T_y_random_ms;
    double T_z_random_ms;
    double T_x_continuous_ms;
    double T_y_continuous_ms;
    double T_z_continuous_ms;
    double T_composite_ms;
    double storageRatio;
    int storageScore;
};

// ========== 核心函数 ==========

// 转换 RAW -> ERWT3D
bool convert(const ConvertConfig& cfg);

// 赛题评分测试
ContestResult benchmarkContest(const BenchConfig& cfg);

// 正确性验证
bool verify(const std::string& rawPath, const std::string& erwt3dPath,
            uint64_t nx, uint64_t ny, uint64_t nz, int samples = 100000);

// ========== 快捷函数 ==========

// HDD 默认配置
inline BenchConfig hddConfig(const std::string& input, const std::string& outputDir) {
    BenchConfig cfg;
    cfg.input = input;
    cfg.outputDir = outputDir;
    cfg.numThreads = 1;
    cfg.memoryLimitMB = 4096;
    cfg.ioBackend = IOBackend::Superblock;
    cfg.readMode = SBReadMode::HDDReadWindow;
    cfg.taskOrder = SBTaskOrder::FileOffset;
    cfg.readWindowBytes = 33554432;
    cfg.maxGapBytes = 1048576;
    cfg.batchPlanner = true;
    cfg.hddMode = true;
    return cfg;
}

} // namespace erwt3d
