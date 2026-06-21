#include "erwt3d/api.hpp"
#include "erwt3d/morton.hpp"
#include <iostream>
#include <fstream>
#include <random>
#include <chrono>
#include <algorithm>
#include <numeric>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

namespace erwt3d {

// ========== 转换 ==========
bool convert(const ConvertConfig& cfg) {
    std::cout << "Converting " << cfg.input << " -> " << cfg.output << std::endl;
    std::cout << "Dimensions: " << cfg.nx << " x " << cfg.ny << " x " << cfg.nz << std::endl;

    bool ok = writeERWT3DFromFile(cfg.output, cfg.input,
                                   cfg.nx, cfg.ny, cfg.nz,
                                   cfg.superSize, cfg.superSize, cfg.superSize,
                                   cfg.leafSize, cfg.leafSize, cfg.leafSize,
                                   cfg.numThreads, cfg.memoryLimitMB,
                                   cfg.panelAxis, cfg.panelStride);
    if (ok) {
        std::cout << "Conversion complete." << std::endl;
    } else {
        std::cerr << "Conversion failed!" << std::endl;
    }
    return ok;
}

// ========== Contest 测试 ==========

struct GroupResult {
    std::string axis, mode;
    int count;
    double groupTimeMs;
};

static bool runGroup(ERWT3DReader& reader, SliceAxis axis, const std::string& axisName,
                     const std::vector<uint64_t>& indices, const std::string& mode,
                     const ERWT3DHeader& header, const BenchConfig& cfg,
                     const std::string& outputDir, GroupResult& result) {
    uint64_t sliceSize;
    switch (axis) {
        case SliceAxis::X: sliceSize = header.ny * header.nz; break;
        case SliceAxis::Y: sliceSize = header.nx * header.nz; break;
        case SliceAxis::Z: sliceSize = header.nx * header.ny; break;
    }
    uint64_t outBytes = sliceSize * sizeof(float);

    result.axis = axisName;
    result.mode = mode;
    result.count = static_cast<int>(indices.size());

    auto groupStart = std::chrono::high_resolution_clock::now();

    if (cfg.batchPlanner && cfg.hddMode) {
        // Batch 模式: 每 20 个切片一批
        const size_t batchSize = 20;
        for (size_t bs = 0; bs < indices.size(); bs += batchSize) {
            size_t be = std::min(bs + batchSize, indices.size());
            size_t bl = be - bs;

            std::vector<ERWT3DReader::SliceBatchRequest> reqs;
            std::vector<std::vector<float>> buffers(bl);
            for (size_t i = 0; i < bl; ++i) {
                buffers[i].resize(sliceSize);
                reqs.push_back({axis, indices[bs + i], buffers[i].data()});
            }

            HDDReadWindowConfig wcfg{cfg.readWindowBytes, cfg.maxGapBytes};
            if (!reader.readSlicesBatch(reqs, cfg.numThreads, cfg.memoryLimitMB, wcfg)) {
                std::cerr << "Batch read failed for " << axisName << std::endl;
                return false;
            }

            for (size_t i = 0; i < bl; ++i) {
                std::string op = outputDir + "/" + axisName + "_" + mode + "_" + std::to_string(bs + i) + ".raw";
                std::ofstream of(op, std::ios::binary);
                of.write(reinterpret_cast<const char*>(buffers[i].data()), outBytes);
            }
        }
    } else {
        // 逐切片模式
        for (size_t i = 0; i < indices.size(); ++i) {
            std::vector<float> output(sliceSize);
            if (!reader.readSlice(axis, indices[i], output.data(), cfg.numThreads, cfg.memoryLimitMB)) {
                std::cerr << "readSlice failed for " << axisName << "[" << indices[i] << "]" << std::endl;
                return false;
            }
            std::string op = outputDir + "/" + axisName + "_" + mode + "_" + std::to_string(i) + ".raw";
            std::ofstream of(op, std::ios::binary);
            of.write(reinterpret_cast<const char*>(output.data()), outBytes);
        }
    }

    auto groupEnd = std::chrono::high_resolution_clock::now();
    result.groupTimeMs = std::chrono::duration<double, std::milli>(groupEnd - groupStart).count();
    return true;
}

ContestResult benchmarkContest(const BenchConfig& cfg) {
    ContestResult result{};

    // 创建输出目录
    std::filesystem::create_directories(cfg.outputDir);

    // 打开 reader
    ERWT3DReader reader(cfg.input, cfg.cacheMB, cfg.hddMode);
    reader.setIOBackend(cfg.ioBackend);
    reader.setSBParallelMode(cfg.parallelMode);
    reader.setSBReadMode(cfg.readMode);
    reader.setSBTaskOrder(cfg.taskOrder);

    if (cfg.readWindowBytes > 0 || cfg.maxGapBytes > 0) {
        reader.setHDDReadWindowConfig({cfg.readWindowBytes, cfg.maxGapBytes});
    }

    const auto& header = reader.getHeader();

    // 生成随机索引
    std::mt19937 rng(cfg.seed);
    std::uniform_int_distribution<uint64_t> distX(0, header.nx - 1);
    std::uniform_int_distribution<uint64_t> distY(0, header.ny - 1);
    std::uniform_int_distribution<uint64_t> distZ(0, header.nz - 1);

    std::vector<uint64_t> randomX(cfg.randomCount), randomY(cfg.randomCount), randomZ(cfg.randomCount);
    for (int i = 0; i < cfg.randomCount; ++i) {
        randomX[i] = distX(rng);
        randomY[i] = distY(rng);
        randomZ[i] = distZ(rng);
    }

    auto safeStart = [](uint64_t dim, int cnt) -> uint64_t {
        if (static_cast<uint64_t>(cnt) >= dim) return 0;
        return dim / 2 - cnt / 2;
    };
    int countX = std::min(cfg.continuousCount, static_cast<int>(header.nx));
    int countY = std::min(cfg.continuousCount, static_cast<int>(header.ny));
    int countZ = std::min(cfg.continuousCount, static_cast<int>(header.nz));
    std::vector<uint64_t> continuousX(countX), continuousY(countY), continuousZ(countZ);
    for (int i = 0; i < countX; ++i) continuousX[i] = safeStart(header.nx, countX) + i;
    for (int i = 0; i < countY; ++i) continuousY[i] = safeStart(header.ny, countY) + i;
    for (int i = 0; i < countZ; ++i) continuousZ[i] = safeStart(header.nz, countZ) + i;

    // 运行 6 组测试
    struct GroupSpec { SliceAxis axis; std::string name, mode; const std::vector<uint64_t>* idxs; };
    std::vector<GroupSpec> groups = {
        {SliceAxis::X, "x", "random", &randomX},
        {SliceAxis::Y, "y", "random", &randomY},
        {SliceAxis::Z, "z", "random", &randomZ},
        {SliceAxis::X, "x", "continuous", &continuousX},
        {SliceAxis::Y, "y", "continuous", &continuousY},
        {SliceAxis::Z, "z", "continuous", &continuousZ},
    };

    double groupTimes[6];
    for (int g = 0; g < 6; ++g) {
        GroupResult gr;
        std::cout << "  [" << (g+1) << "/6] " << groups[g].name << " " << groups[g].mode
                  << " (" << groups[g].idxs->size() << " slices)..." << std::flush;

        if (!runGroup(reader, groups[g].axis, groups[g].name, *groups[g].idxs, groups[g].mode,
                      header, cfg, cfg.outputDir, gr)) {
            std::cerr << " FAILED" << std::endl;
            return result;
        }

        std::cout << " " << std::fixed << std::setprecision(4) << gr.groupTimeMs / 1000.0 << "s"
                  << " (avg=" << std::setprecision(4) << gr.groupTimeMs / gr.count / 1000.0 << "s)" << std::endl;
        groupTimes[g] = gr.groupTimeMs;
    }

    // 计算结果
    result.T_x_random_ms = groupTimes[0];
    result.T_y_random_ms = groupTimes[1];
    result.T_z_random_ms = groupTimes[2];
    result.T_x_continuous_ms = groupTimes[3];
    result.T_y_continuous_ms = groupTimes[4];
    result.T_z_continuous_ms = groupTimes[5];

    double total = 0;
    for (int g = 0; g < 6; ++g) total += groupTimes[g];
    result.T_composite_ms = total / 6.0;

    // 存储比例
    uint64_t rawBytes = getRawSize(header);
    struct stat st;
    if (stat(cfg.input.c_str(), &st) == 0) {
        result.storageRatio = static_cast<double>(st.st_size) / rawBytes;
        result.storageScore = 20;
        if (result.storageRatio > 1.5) {
            double over = result.storageRatio - 1.5;
            int penalty = static_cast<int>(std::ceil(over / 0.1));
            result.storageScore = std::max(0, 20 - penalty);
        }
    }

    // 输出汇总
    std::cout << "\n==================== 结果 ====================" << std::endl;
    std::cout << "  T_x_random:     " << std::fixed << std::setprecision(4) << result.T_x_random_ms / 1000.0 << "s" << std::endl;
    std::cout << "  T_y_random:     " << result.T_y_random_ms / 1000.0 << "s" << std::endl;
    std::cout << "  T_z_random:     " << result.T_z_random_ms / 1000.0 << "s" << std::endl;
    std::cout << "  T_x_continuous: " << result.T_x_continuous_ms / 1000.0 << "s" << std::endl;
    std::cout << "  T_y_continuous: " << result.T_y_continuous_ms / 1000.0 << "s" << std::endl;
    std::cout << "  T_z_continuous: " << result.T_z_continuous_ms / 1000.0 << "s" << std::endl;
    std::cout << "  T_composite:    " << result.T_composite_ms / 1000.0 << "s" << std::endl;
    std::cout << "  Storage:        " << std::setprecision(3) << result.storageRatio << "x (" << result.storageScore << "/20)" << std::endl;
    std::cout << "==============================================" << std::endl;

    return result;
}

// ========== 验证 ==========
bool verify(const std::string& rawPath, const std::string& erwt3dPath,
            uint64_t nx, uint64_t ny, uint64_t nz, int samples) {
    std::cout << "Verifying " << erwt3dPath << " against " << rawPath << std::endl;

    // 打开文件
    int rawFd = open(rawPath.c_str(), O_RDONLY);
    int erwt3dFd = open(erwt3dPath.c_str(), O_RDONLY);
    if (rawFd < 0 || erwt3dFd < 0) {
        std::cerr << "Cannot open files" << std::endl;
        if (rawFd >= 0) close(rawFd);
        if (erwt3dFd >= 0) close(erwt3dFd);
        return false;
    }

    // 读取 header
    ERWT3DHeader header;
    if (read(erwt3dFd, &header, sizeof(header)) != sizeof(header)) {
        close(rawFd); close(erwt3dFd);
        return false;
    }

    // 构建 reader
    ERWT3DReader reader(erwt3dPath);
    if (!reader.getHeader().nx) {
        close(rawFd); close(erwt3dFd);
        return false;
    }

    // 随机采样验证
    std::mt19937 rng(42);
    std::uniform_int_distribution<uint64_t> distX(0, nx - 1);
    std::uniform_int_distribution<uint64_t> distY(0, ny - 1);
    std::uniform_int_distribution<uint64_t> distZ(0, nz - 1);

    uint64_t rawSize = nx * ny * nz * sizeof(float);

    // mmap 原始文件
    struct stat st;
    fstat(rawFd, &st);
    const float* rawData = static_cast<const float*>(
        mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, rawFd, 0));

    if (rawData == MAP_FAILED) {
        std::cerr << "mmap failed" << std::endl;
        close(rawFd); close(erwt3dFd);
        return false;
    }

    int failed = 0;
    double maxAbsErr = 0, maxRelErr = 0;

    for (int s = 0; s < samples; ++s) {
        uint64_t x = distX(rng), y = distY(rng), z = distZ(rng);
        uint64_t idx = (z * ny + y) * nx + x;
        float expected = rawData[idx];

        // 从 erwt3d 读取包含该点的 Z 切片
        std::vector<float> slice(nx * ny);
        reader.readSlice(SliceAxis::Z, z, slice.data(), 1, 2048);
        float actual = slice[y * nx + x];

        double absErr = std::abs(actual - expected);
        double relErr = expected != 0 ? absErr / std::abs(expected) : absErr;

        maxAbsErr = std::max(maxAbsErr, absErr);
        maxRelErr = std::max(maxRelErr, relErr);

        if (relErr > 0.001) {
            failed++;
        }
    }

    munmap(const_cast<float*>(rawData), st.st_size);
    close(rawFd);
    close(erwt3dFd);

    std::cout << "  Samples:      " << samples << std::endl;
    std::cout << "  Max abs err:  " << std::scientific << maxAbsErr << std::endl;
    std::cout << "  Max rel err:  " << maxRelErr << std::endl;
    std::cout << "  Failed:       " << failed << std::endl;
    std::cout << "  Result:       " << (failed == 0 ? "PASSED" : "FAILED") << std::endl;

    return failed == 0;
}

} // namespace erwt3d
