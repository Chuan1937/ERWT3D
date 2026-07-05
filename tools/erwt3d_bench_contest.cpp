#include "erwt3d/reader.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

enum class TimingMode { Strict, Fast };
enum class ContinuousStartMode { Random, Middle, Zero };

struct GroupResult {
    std::string axis;
    std::string mode;
    int sliceCount = 0;
    double groupTimeMs = 0.0;
    uint64_t outputBytesPerSlice = 0;
    std::vector<double> perSliceTimes;
};

struct GroupAggregate {
    GroupResult bestResult;
    double minMs = 0.0;
    double meanMs = 0.0;
    double medianMs = 0.0;
    double maxMs = 0.0;
};

struct BenchOptions {
    std::string inputPath;
    std::string outputDir;
    std::string baselineFile;
    std::string storagePath;
    int randomCount = 100;
    int continuousCount = 10;
    int numThreads = 1;
    size_t memoryLimitMB = 2048;
    size_t cacheMB = 0;
    uint32_t seed = 20260511;
    std::string ioBackendStr = "sb";
    std::string sbReadModeStr = "hdd-read-window";
    std::string sbTaskOrderStr = "file-offset";
    uint64_t hddReadWindowBytes = 0;
    uint64_t hddMaxGapBytes = 0;
    bool pinThreads = false;
    bool dryRun = false;
    bool useBatch = true;
    bool useMmap = false;
    bool hddMode = false;
    bool fsyncOutput = false;
    double baselineMsOverride = 0.0;
    int repeats = 1;
    TimingMode timingMode = TimingMode::Strict;
    ContinuousStartMode continuousStartMode = ContinuousStartMode::Random;
};

struct StorageStats {
    uint64_t fileBytes = 0;
    uint64_t storageBytes = 0;
    uint64_t rawBytes = 0;
    double storageRatio = 0.0;
    int storageScore = 20;
    std::string storagePath;
};

struct GroupSpec {
    erwt3d::SliceAxis axis;
    std::string axisName;
    std::string mode;
    const std::vector<uint64_t>* indices;
};

std::string timingModeName(TimingMode mode) {
    return mode == TimingMode::Strict ? "strict" : "fast";
}

std::string continuousStartModeName(ContinuousStartMode mode) {
    switch (mode) {
        case ContinuousStartMode::Random: return "random";
        case ContinuousStartMode::Middle: return "middle";
        case ContinuousStartMode::Zero: return "zero";
    }
    return "random";
}

void printUsage(const char* prog) {
    std::cerr << "Usage: " << prog << " --input data.erwt3d --output-dir DIR [options]\n\n"
              << "Competition benchmark aligned with赛题2 timing and storage rules.\n\n"
              << "  T_composite = (T_xr + T_yr + T_zr + T_xc + T_yc + T_zc) / 6\n"
              << "  Score = (baseline / T_composite) × 60\n\n"
              << "Options:\n"
              << "  --input PATH                ERWT3D file (required)\n"
              << "  --output-dir DIR            Output directory (required)\n"
              << "  --storage-path PATH         File or directory counted for storage score (default: --input)\n"
              << "  --random-count N            Random slices per axis (default: 100)\n"
              << "  --continuous-count N        Continuous slices per axis (default: 10)\n"
              << "  --continuous-start MODE     random|middle|zero (default: random)\n"
              << "  --timing-mode MODE          strict|fast (default: strict)\n"
              << "  --fsync-output              Call fsync(fd) before close\n"
              << "  --threads N|auto            Thread count (default: 1)\n"
              << "  --memory-limit-mb N         Memory limit (default: 2048)\n"
              << "  --cache-mb N                LRU cache size (default: 0)\n"
              << "  --io-backend pread|sb       I/O backend (default: sb)\n"
              << "  --sb-read-mode MODE         run-batch|leaf-index|hdd-read-window (default: hdd-read-window)\n"
              << "  --sb-task-order MODE        logical|file-offset (default: file-offset)\n"
              << "  --hdd-read-window-bytes N   HDD read window (0=auto, default: 0)\n"
              << "  --hdd-max-gap-bytes N       HDD max gap to merge (default: 0)\n"
              << "  --pin-threads               Pin threads to CPU cores\n"
              << "  --seed N                    Random seed (default: 20260511)\n"
              << "  --dry-run                   Print plan only, skip actual reads\n"
              << "  --baseline-ms N             Baseline T_composite for score calc\n"
              << "  --baseline-file PATH        Read baseline T_composite from CSV\n"
              << "  --repeats N                 Repeat each group N times, take min (default: 1)\n"
              << "  --hdd                       Enable HDD-oriented defaults\n";
}

double medianOf(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    size_t mid = values.size() / 2;
    if (values.size() % 2 == 0) {
        return (values[mid - 1] + values[mid]) / 2.0;
    }
    return values[mid];
}

bool writeBufferToFd(int fd, const float* data, uint64_t bytes) {
    const char* ptr = reinterpret_cast<const char*>(data);
    uint64_t writtenTotal = 0;
    while (writtenTotal < bytes) {
        ssize_t written = pwrite(fd, ptr + writtenTotal, bytes - writtenTotal, writtenTotal);
        if (written <= 0) {
            return false;
        }
        writtenTotal += static_cast<uint64_t>(written);
    }
    return true;
}

uint64_t computeStorageBytes(const std::string& path) {
    std::error_code ec;
    auto st = std::filesystem::status(path, ec);
    if (ec) {
        return 0;
    }
    if (std::filesystem::is_regular_file(st)) {
        return std::filesystem::file_size(path, ec);
    }
    if (!std::filesystem::is_directory(st)) {
        return 0;
    }

    uint64_t total = 0;
    for (std::filesystem::recursive_directory_iterator it(path, ec), end; it != end && !ec; it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            total += it->file_size(ec);
        }
    }
    return ec ? 0 : total;
}

StorageStats computeStorageStats(const BenchOptions& opt, const erwt3d::ERWT3DHeader& header) {
    StorageStats stats;
    stats.rawBytes = erwt3d::getRawSize(header);
    stats.storagePath = opt.storagePath.empty() ? opt.inputPath : opt.storagePath;

    struct stat fileStat;
    if (stat(opt.inputPath.c_str(), &fileStat) == 0) {
        stats.fileBytes = static_cast<uint64_t>(fileStat.st_size);
    }

    stats.storageBytes = computeStorageBytes(stats.storagePath);
    if (stats.storageBytes == 0 && !stats.storagePath.empty()) {
        stats.storageBytes = stats.fileBytes;
    }

    stats.storageRatio = stats.rawBytes == 0
        ? 0.0
        : static_cast<double>(stats.storageBytes) / static_cast<double>(stats.rawBytes);
    if (stats.storageRatio > 1.5) {
        double over = stats.storageRatio - 1.5;
        int penalty = static_cast<int>(std::ceil(over / 0.1));
        stats.storageScore = std::max(0, 20 - penalty);
    }
    return stats;
}

uint64_t chooseContinuousStart(uint64_t dim, int count, ContinuousStartMode mode, std::mt19937& rng) {
    if (count <= 0 || static_cast<uint64_t>(count) >= dim) {
        return 0;
    }
    switch (mode) {
        case ContinuousStartMode::Random: {
            std::uniform_int_distribution<uint64_t> dist(0, dim - static_cast<uint64_t>(count));
            return dist(rng);
        }
        case ContinuousStartMode::Middle:
            return dim / 2 - static_cast<uint64_t>(count) / 2;
        case ContinuousStartMode::Zero:
            return 0;
    }
    return 0;
}

bool closeGroupFds(std::vector<int>& fds, bool fsyncOutput) {
    for (int fd : fds) {
        if (fd < 0) {
            continue;
        }
        if (fsyncOutput && fsync(fd) != 0) {
            return false;
        }
        if (close(fd) != 0) {
            return false;
        }
    }
    return true;
}

bool prepareFastOutputs(const std::string& outputDir,
                        const std::string& axisName,
                        const std::string& mode,
                        size_t count,
                        std::vector<int>& fds) {
    fds.assign(count, -1);
    for (size_t i = 0; i < count; ++i) {
        std::string outPath = outputDir + "/" + axisName + "_" + mode + "_" + std::to_string(i) + ".raw";
        int fd = open(outPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            std::cerr << "\nError: Cannot pre-create " << outPath << "\n";
            return false;
        }
        fds[i] = fd;
    }
    return true;
}

bool runGroup(erwt3d::ERWT3DReader& reader,
              const GroupSpec& spec,
              const erwt3d::ERWT3DHeader& header,
              const BenchOptions& opt,
              GroupResult& result) {
    uint64_t sliceSize = 0;
    switch (spec.axis) {
        case erwt3d::SliceAxis::X: sliceSize = header.ny * header.nz; break;
        case erwt3d::SliceAxis::Y: sliceSize = header.nx * header.nz; break;
        case erwt3d::SliceAxis::Z: sliceSize = header.nx * header.ny; break;
    }
    uint64_t outBytes = sliceSize * sizeof(float);
    const auto& indices = *spec.indices;

    result.axis = spec.axisName;
    result.mode = spec.mode;
    result.sliceCount = static_cast<int>(indices.size());
    result.outputBytesPerSlice = outBytes;
    result.perSliceTimes.assign(indices.size(), 0.0);

    erwt3d::HDDReadWindowConfig wcfg{opt.hddReadWindowBytes, opt.hddMaxGapBytes};
    std::vector<int> fds;
    if (opt.timingMode == TimingMode::Fast && !prepareFastOutputs(opt.outputDir, spec.axisName, spec.mode, indices.size(), fds)) {
        return false;
    }

    auto groupStart = std::chrono::high_resolution_clock::now();

    if (opt.timingMode == TimingMode::Strict) {
        if (!prepareFastOutputs(opt.outputDir, spec.axisName, spec.mode, indices.size(), fds)) {
            return false;
        }
    }

    bool ok = true;
    if (opt.useBatch) {
        size_t totalSlices = indices.size();
        uint64_t sbBytes = static_cast<uint64_t>(header.super_x) * header.super_y * header.super_z * sizeof(float);
        uint64_t readWindow = wcfg.read_window_bytes > 0 ? wcfg.read_window_bytes : 128ULL * 1024 * 1024ULL;
        size_t readBufPerThread = static_cast<size_t>(
            std::min<uint64_t>(opt.memoryLimitMB * 1024ULL * 1024ULL / std::max(opt.numThreads, 1),
                               std::max(readWindow, sbBytes * 4)));
        size_t totalReadBufMB = readBufPerThread * std::max(opt.numThreads, 1) / (1024ULL * 1024ULL);
        size_t outputBudgetMB = (opt.memoryLimitMB > totalReadBufMB + 64)
            ? opt.memoryLimitMB - totalReadBufMB - 64
            : opt.memoryLimitMB / 2;
        size_t maxBatch = (outputBudgetMB * 1024ULL * 1024ULL) / (outBytes + 1);
        if (maxBatch < 1) {
            maxBatch = 1;
        }
        size_t batchSize = std::min(totalSlices, maxBatch);

        for (size_t batchStart = 0; batchStart < totalSlices && ok; batchStart += batchSize) {
            size_t batchEnd = std::min(batchStart + batchSize, totalSlices);
            size_t batchLen = batchEnd - batchStart;
            std::vector<erwt3d::ERWT3DReader::SliceBatchRequest> reqs;
            std::vector<std::vector<float>> buffers(batchLen);
            reqs.reserve(batchLen);

            for (size_t i = 0; i < batchLen; ++i) {
                buffers[i].resize(sliceSize);
                reqs.push_back({spec.axis, indices[batchStart + i], buffers[i].data()});
            }

            if (!reader.readSlicesBatch(reqs, opt.numThreads, opt.memoryLimitMB, wcfg)) {
                std::cerr << "\nError: batch read failed for " << spec.axisName << "\n";
                ok = false;
                break;
            }

            for (size_t i = 0; i < batchLen; ++i) {
                if (!writeBufferToFd(fds[batchStart + i], buffers[i].data(), outBytes)) {
                    std::cerr << "\nError: Write failed for " << spec.axisName << "[" << (batchStart + i) << "]\n";
                    ok = false;
                    break;
                }
            }
        }
    } else {
        std::vector<float> output(sliceSize);
        for (size_t i = 0; i < indices.size() && ok; ++i) {
            if (!reader.readSlice(spec.axis, indices[i], output.data(), opt.numThreads, opt.memoryLimitMB)) {
                std::cerr << "\nError: readSlice failed for " << spec.axisName << "[" << indices[i] << "]\n";
                ok = false;
                break;
            }
            if (!writeBufferToFd(fds[i], output.data(), outBytes)) {
                std::cerr << "\nError: Write failed for " << spec.axisName << "[" << i << "]\n";
                ok = false;
                break;
            }
        }
    }

    if (ok) {
        if (!closeGroupFds(fds, opt.fsyncOutput)) {
            std::cerr << "\nError: failed to finalize output files for " << spec.axisName << " " << spec.mode << "\n";
            ok = false;
        }
    } else {
        closeGroupFds(fds, false);
    }

    auto groupEnd = std::chrono::high_resolution_clock::now();
    result.groupTimeMs = std::chrono::duration<double, std::milli>(groupEnd - groupStart).count();
    return ok;
}

double meanOf(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }
    return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
}

}  // namespace

int main(int argc, char* argv[]) {
    BenchOptions opt;

    for (int i = 1; i < argc; ++i) {
        auto next = [&]() -> const char* {
            if (i + 1 < argc) {
                return argv[++i];
            }
            std::cerr << "Error: " << argv[i] << " requires a value\n";
            std::exit(1);
            return nullptr;
        };

        if (std::strcmp(argv[i], "--input") == 0 || std::strcmp(argv[i], "-i") == 0) { opt.inputPath = next(); }
        else if (std::strcmp(argv[i], "--output-dir") == 0 || std::strcmp(argv[i], "-o") == 0) { opt.outputDir = next(); }
        else if (std::strcmp(argv[i], "--storage-path") == 0) { opt.storagePath = next(); }
        else if (std::strcmp(argv[i], "--random-count") == 0) { opt.randomCount = std::stoi(next()); }
        else if (std::strcmp(argv[i], "--continuous-count") == 0) { opt.continuousCount = std::stoi(next()); }
        else if (std::strcmp(argv[i], "--continuous-start") == 0) {
            std::string v = next();
            if (v == "random") opt.continuousStartMode = ContinuousStartMode::Random;
            else if (v == "middle") opt.continuousStartMode = ContinuousStartMode::Middle;
            else if (v == "zero") opt.continuousStartMode = ContinuousStartMode::Zero;
            else { std::cerr << "Error: invalid --continuous-start: " << v << "\n"; return 1; }
        }
        else if (std::strcmp(argv[i], "--timing-mode") == 0) {
            std::string v = next();
            if (v == "strict") opt.timingMode = TimingMode::Strict;
            else if (v == "fast") opt.timingMode = TimingMode::Fast;
            else { std::cerr << "Error: invalid --timing-mode: " << v << "\n"; return 1; }
        }
        else if (std::strcmp(argv[i], "--fsync-output") == 0) { opt.fsyncOutput = true; }
        else if (std::strcmp(argv[i], "--threads") == 0 || std::strcmp(argv[i], "-t") == 0) {
            const char* v = next();
            if (std::strcmp(v, "auto") == 0) {
                unsigned hw = std::thread::hardware_concurrency();
                opt.numThreads = static_cast<int>(std::min(std::max(1u, hw / 2), 8u));
            } else {
                opt.numThreads = std::stoi(v);
            }
        }
        else if (std::strcmp(argv[i], "--memory-limit-mb") == 0 || std::strcmp(argv[i], "-m") == 0) { opt.memoryLimitMB = std::stoul(next()); }
        else if (std::strcmp(argv[i], "--cache-mb") == 0) { opt.cacheMB = std::stoul(next()); }
        else if (std::strcmp(argv[i], "--io-backend") == 0) { opt.ioBackendStr = next(); }
        else if (std::strcmp(argv[i], "--sb-read-mode") == 0) { opt.sbReadModeStr = next(); }
        else if (std::strcmp(argv[i], "--sb-task-order") == 0) { opt.sbTaskOrderStr = next(); }
        else if (std::strcmp(argv[i], "--hdd-read-window-bytes") == 0) { opt.hddReadWindowBytes = std::stoull(next()); }
        else if (std::strcmp(argv[i], "--hdd-max-gap-bytes") == 0) { opt.hddMaxGapBytes = std::stoull(next()); }
        else if (std::strcmp(argv[i], "--pin-threads") == 0) { opt.pinThreads = true; }
        else if (std::strcmp(argv[i], "--seed") == 0) { opt.seed = std::stoul(next()); }
        else if (std::strcmp(argv[i], "--dry-run") == 0) { opt.dryRun = true; }
        else if (std::strcmp(argv[i], "--batch") == 0) { opt.useBatch = true; }
        else if (std::strcmp(argv[i], "--mmap") == 0) { opt.useMmap = true; }
        else if (std::strcmp(argv[i], "--hdd") == 0) {
            opt.hddMode = true;
            opt.numThreads = 1;
            opt.memoryLimitMB = 4096;
            opt.ioBackendStr = "sb";
            opt.sbReadModeStr = "hdd-read-window";
            opt.sbTaskOrderStr = "file-offset";
            opt.hddReadWindowBytes = 134217728;
            opt.hddMaxGapBytes = 3145728;
            opt.useBatch = true;
        }
        else if (std::strcmp(argv[i], "--baseline-ms") == 0) { opt.baselineMsOverride = std::stod(next()); }
        else if (std::strcmp(argv[i], "--baseline-file") == 0) { opt.baselineFile = next(); }
        else if (std::strcmp(argv[i], "--repeats") == 0) { opt.repeats = std::stoi(next()); }
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) { printUsage(argv[0]); return 0; }
        else { std::cerr << "Unknown option: " << argv[i] << "\n"; printUsage(argv[0]); return 1; }
    }

    if (opt.inputPath.empty() || opt.outputDir.empty()) {
        std::cerr << "Error: --input and --output-dir are required\n";
        printUsage(argv[0]);
        return 1;
    }
    if (opt.randomCount < 0 || opt.continuousCount < 0 || opt.repeats < 1) {
        std::cerr << "Error: counts and repeats must be non-negative, repeats >= 1\n";
        return 1;
    }

    std::error_code ec;
    std::filesystem::create_directories(opt.outputDir, ec);
    if (ec) {
        std::cerr << "Error: " << ec.message() << "\n";
        return 1;
    }

    erwt3d::ERWT3DReader reader(opt.inputPath, opt.cacheMB, opt.useMmap);
    if (opt.ioBackendStr == "sb" || opt.ioBackendStr == "superblock") {
        reader.setIOBackend(erwt3d::IOBackend::Superblock);
    }
    if (opt.sbReadModeStr == "run-batch") reader.setSBReadMode(erwt3d::SBReadMode::RunBatch);
    else if (opt.sbReadModeStr == "leaf-index") reader.setSBReadMode(erwt3d::SBReadMode::LeafIndex);
    else if (opt.sbReadModeStr == "hdd-read-window") reader.setSBReadMode(erwt3d::SBReadMode::HDDReadWindow);
    if (opt.sbTaskOrderStr == "file-offset") reader.setSBTaskOrder(erwt3d::SBTaskOrder::FileOffset);
    if (opt.hddReadWindowBytes > 0 || opt.hddMaxGapBytes > 0) {
        reader.setHDDReadWindowConfig({opt.hddReadWindowBytes, opt.hddMaxGapBytes});
    }
    reader.setPinThreads(opt.pinThreads);

    const auto& header = reader.getHeader();
    StorageStats storage = computeStorageStats(opt, header);

    std::cout << "============================================================\n"
              << "  ERWT3D Competition Benchmark (赛题2 评分标准)\n"
              << "============================================================\n"
              << "  File:        " << opt.inputPath << "\n"
              << "  Dims:        " << header.nx << " x " << header.ny << " x " << header.nz << "\n"
              << "  Random:      " << opt.randomCount << " slices/axis\n"
              << "  Continuous:  " << opt.continuousCount << " slices/axis\n"
              << "  Threads:     " << opt.numThreads << "\n"
              << "  MemLimit:    " << opt.memoryLimitMB << " MB\n"
              << "  Cache:       " << opt.cacheMB << " MB\n"
              << "  Backend:     " << opt.ioBackendStr << "\n"
              << "  ReadMode:    " << opt.sbReadModeStr << "\n"
              << "  TaskOrd:     " << opt.sbTaskOrderStr << "\n"
              << "  TimingMode:  " << timingModeName(opt.timingMode) << "\n"
              << "  FsyncOutput: " << (opt.fsyncOutput ? "true" : "false") << "\n"
              << "  Repeats:     " << opt.repeats << "\n"
              << "  StoragePath: " << storage.storagePath << "\n"
              << "  Storage:     " << storage.storageBytes << " bytes (" << std::fixed << std::setprecision(3)
              << storage.storageRatio << "x) -> " << storage.storageScore << "/20 pts\n"
              << "============================================================\n\n";

    std::mt19937 rng(opt.seed);
    std::uniform_int_distribution<uint64_t> distX(0, header.nx - 1);
    std::uniform_int_distribution<uint64_t> distY(0, header.ny - 1);
    std::uniform_int_distribution<uint64_t> distZ(0, header.nz - 1);

    std::vector<uint64_t> randomX(opt.randomCount), randomY(opt.randomCount), randomZ(opt.randomCount);
    for (int i = 0; i < opt.randomCount; ++i) {
        randomX[i] = distX(rng);
        randomY[i] = distY(rng);
        randomZ[i] = distZ(rng);
    }

    int countX = std::min(opt.continuousCount, static_cast<int>(header.nx));
    int countY = std::min(opt.continuousCount, static_cast<int>(header.ny));
    int countZ = std::min(opt.continuousCount, static_cast<int>(header.nz));
    uint64_t sx = chooseContinuousStart(header.nx, countX, opt.continuousStartMode, rng);
    uint64_t sy = chooseContinuousStart(header.ny, countY, opt.continuousStartMode, rng);
    uint64_t sz = chooseContinuousStart(header.nz, countZ, opt.continuousStartMode, rng);

    std::vector<uint64_t> continuousX(countX), continuousY(countY), continuousZ(countZ);
    for (int i = 0; i < countX; ++i) continuousX[i] = sx + static_cast<uint64_t>(i);
    for (int i = 0; i < countY; ++i) continuousY[i] = sy + static_cast<uint64_t>(i);
    for (int i = 0; i < countZ; ++i) continuousZ[i] = sz + static_cast<uint64_t>(i);

    if (opt.dryRun) {
        std::cout << "[DRY RUN] Would benchmark 6 groups:\n"
                  << "  X random:       " << opt.randomCount << " slices, dim=" << header.ny << "x" << header.nz << "\n"
                  << "  Y random:       " << opt.randomCount << " slices, dim=" << header.nx << "x" << header.nz << "\n"
                  << "  Z random:       " << opt.randomCount << " slices, dim=" << header.nx << "x" << header.ny << "\n"
                  << "  X continuous:   " << countX << " slices starting at " << sx << "\n"
                  << "  Y continuous:   " << countY << " slices starting at " << sy << "\n"
                  << "  Z continuous:   " << countZ << " slices starting at " << sz << "\n"
                  << "  Timing mode:    " << timingModeName(opt.timingMode) << "\n"
                  << "  Continuous mode:" << continuousStartModeName(opt.continuousStartMode) << "\n"
                  << "  Storage:        " << storage.storageRatio << "x -> " << storage.storageScore << "/20\n";
        return 0;
    }

    std::vector<GroupSpec> groups = {
        {erwt3d::SliceAxis::X, "x", "random", &randomX},
        {erwt3d::SliceAxis::Y, "y", "random", &randomY},
        {erwt3d::SliceAxis::Z, "z", "random", &randomZ},
        {erwt3d::SliceAxis::X, "x", "continuous", &continuousX},
        {erwt3d::SliceAxis::Y, "y", "continuous", &continuousY},
        {erwt3d::SliceAxis::Z, "z", "continuous", &continuousZ},
    };

    if (opt.repeats > 1) {
        std::cout << "Warning: repeats > 1 uses min group time for T_composite. This is useful for tuning but may be optimistic for contest reporting.\n\n";
    }

    std::vector<GroupAggregate> aggregates(6);
    double groupTimes[6] = {};

    std::cout << "Running 6 benchmark groups...\n\n";
    for (int g = 0; g < 6; ++g) {
        const auto& spec = groups[g];
        std::vector<double> repeatTimes;
        GroupResult bestResult;
        double bestMs = std::numeric_limits<double>::max();

        for (int rep = 0; rep < opt.repeats; ++rep) {
            GroupResult gr;
            std::cout << "  [" << (g + 1) << "/6] " << spec.axisName << " " << spec.mode
                      << " (" << spec.indices->size() << " slices)";
            if (opt.repeats > 1) {
                std::cout << " rep=" << rep;
            }
            std::cout << "..." << std::flush;

            if (!runGroup(reader, spec, header, opt, gr)) {
                return 1;
            }

            repeatTimes.push_back(gr.groupTimeMs);
            std::cout << " " << std::fixed << std::setprecision(4) << gr.groupTimeMs / 1000.0 << "s"
                      << " (avg=" << std::setprecision(4) << gr.groupTimeMs / std::max(gr.sliceCount, 1) / 1000.0 << "s)\n";

            if (gr.groupTimeMs < bestMs) {
                bestMs = gr.groupTimeMs;
                bestResult = gr;
            }
        }

        aggregates[g].bestResult = bestResult;
        aggregates[g].minMs = *std::min_element(repeatTimes.begin(), repeatTimes.end());
        aggregates[g].meanMs = meanOf(repeatTimes);
        aggregates[g].medianMs = medianOf(repeatTimes);
        aggregates[g].maxMs = *std::max_element(repeatTimes.begin(), repeatTimes.end());
        groupTimes[g] = aggregates[g].minMs;
    }

    double totalAllGroups = 0.0;
    for (double t : groupTimes) {
        totalAllGroups += t;
    }
    double tComposite = totalAllGroups / 6.0;
    double avgX = (groupTimes[0] + groupTimes[3]) / 2.0;
    double avgY = (groupTimes[1] + groupTimes[4]) / 2.0;
    double avgZ = (groupTimes[2] + groupTimes[5]) / 2.0;

    double baselineMs = opt.baselineMsOverride;
    if (baselineMs <= 0 && !opt.baselineFile.empty()) {
        std::ifstream bf(opt.baselineFile);
        if (bf) {
            std::string line;
            while (std::getline(bf, line)) {
                if (line.find("T_composite_ms,") == 0) {
                    baselineMs = std::stod(line.substr(15));
                    break;
                }
            }
        }
    }

    std::string detailPath = opt.outputDir + "/contest_detail.csv";
    {
        std::ofstream df(detailPath);
        df << "group,axis,mode,iteration,slice_index,time_ms,output_bytes\n";
        for (int g = 0; g < 6; ++g) {
            const auto& r = aggregates[g].bestResult;
            const auto& idxVec = (r.mode == "random")
                ? (r.axis == "x" ? randomX : r.axis == "y" ? randomY : randomZ)
                : (r.axis == "x" ? continuousX : r.axis == "y" ? continuousY : continuousZ);
            for (size_t i = 0; i < r.perSliceTimes.size(); ++i) {
                df << g << "," << r.axis << "," << r.mode << "," << i << ","
                   << idxVec[i] << "," << std::fixed << std::setprecision(3) << r.perSliceTimes[i]
                   << "," << r.outputBytesPerSlice << "\n";
            }
        }
    }

    std::string summaryPath = opt.outputDir + "/contest_summary.csv";
    {
        std::ofstream sf(summaryPath);
        sf << "group,axis,mode,slice_count,group_time_ms,group_time_min_ms,group_time_mean_ms,group_time_median_ms,group_time_max_ms,avg_per_slice_ms,output_bytes_per_slice\n";
        for (int g = 0; g < 6; ++g) {
            const auto& agg = aggregates[g];
            const auto& r = agg.bestResult;
            sf << g << "," << r.axis << "," << r.mode << "," << r.sliceCount << ","
               << std::fixed << std::setprecision(3)
               << r.groupTimeMs << "," << agg.minMs << "," << agg.meanMs << "," << agg.medianMs << "," << agg.maxMs << ","
               << (r.sliceCount > 0 ? r.groupTimeMs / r.sliceCount : 0.0) << "," << r.outputBytesPerSlice << "\n";
        }
    }

    std::string scorePath = opt.outputDir + "/contest_score.csv";
    {
        std::ofstream sc(scorePath);
        sc << "metric,value\n"
           << "input_file," << opt.inputPath << "\n"
           << "dimensions," << header.nx << "x" << header.ny << "x" << header.nz << "\n"
           << "random_count," << opt.randomCount << "\n"
           << "continuous_count," << opt.continuousCount << "\n"
           << "continuous_start_mode," << continuousStartModeName(opt.continuousStartMode) << "\n"
           << "continuous_start_x," << sx << "\n"
           << "continuous_start_y," << sy << "\n"
           << "continuous_start_z," << sz << "\n"
           << "threads," << opt.numThreads << "\n"
           << "memory_limit_mb," << opt.memoryLimitMB << "\n"
           << "io_backend," << opt.ioBackendStr << "\n"
           << "sb_read_mode," << opt.sbReadModeStr << "\n"
           << "timing_mode," << timingModeName(opt.timingMode) << "\n"
           << "fsync_output," << (opt.fsyncOutput ? "true" : "false") << "\n"
           << "repeats," << opt.repeats << "\n"
           << "repeat_reduce,min\n"
           << "storage_path," << storage.storagePath << "\n"
           << std::fixed << std::setprecision(3)
           << "T_x_random_ms," << groupTimes[0] << "\n"
           << "T_y_random_ms," << groupTimes[1] << "\n"
           << "T_z_random_ms," << groupTimes[2] << "\n"
           << "T_x_continuous_ms," << groupTimes[3] << "\n"
           << "T_y_continuous_ms," << groupTimes[4] << "\n"
           << "T_z_continuous_ms," << groupTimes[5] << "\n"
           << "total_all_groups_ms," << totalAllGroups << "\n"
           << "T_composite_ms," << tComposite << "\n"
           << "avg_X_ms," << avgX << "\n"
           << "avg_Y_ms," << avgY << "\n"
           << "avg_Z_ms," << avgZ << "\n"
           << "file_bytes," << storage.fileBytes << "\n"
           << "storage_bytes," << storage.storageBytes << "\n"
           << "raw_bytes," << storage.rawBytes << "\n"
           << "storage_ratio," << storage.storageRatio << "\n"
           << "storage_score," << storage.storageScore << "\n";
    }

    std::cout << "\n============================================================\n"
              << "  COMPETITION SCORE (赛题2 评分标准)\n"
              << "============================================================\n\n"
              << "  6 Group Times (wall-clock, read + write):\n"
              << "    [1] X random:      " << std::setw(8) << std::fixed << std::setprecision(4) << groupTimes[0] / 1000.0 << "s  (" << opt.randomCount << " slices)\n"
              << "    [2] Y random:      " << std::setw(8) << groupTimes[1] / 1000.0 << "s\n"
              << "    [3] Z random:      " << std::setw(8) << groupTimes[2] / 1000.0 << "s\n"
              << "    [4] X continuous:  " << std::setw(8) << groupTimes[3] / 1000.0 << "s  (" << countX << " slices)\n"
              << "    [5] Y continuous:  " << std::setw(8) << groupTimes[4] / 1000.0 << "s\n"
              << "    [6] Z continuous:  " << std::setw(8) << groupTimes[5] / 1000.0 << "s\n"
              << "    ----------------------------------------\n"
              << "    Total:             " << std::setw(8) << totalAllGroups / 1000.0 << "s\n"
              << "    T_composite = total/6 = " << tComposite / 1000.0 << "s\n\n"
              << "  Timing options:\n"
              << "    timing_mode = " << timingModeName(opt.timingMode) << "\n"
              << "    fsync_output = " << (opt.fsyncOutput ? "true" : "false") << "\n\n"
              << "  Continuous starts:\n"
              << "    mode = " << continuousStartModeName(opt.continuousStartMode) << "\n"
              << "    x = " << sx << ", y = " << sy << ", z = " << sz << "\n\n"
              << "  Per-axis diagnostics:\n"
              << "    avg X = (random+cont)/2 = " << avgX / 1000.0 << "s\n"
              << "    avg Y = " << avgY / 1000.0 << "s\n"
              << "    avg Z = " << avgZ / 1000.0 << "s\n\n"
              << "  Storage (20pts):\n"
              << "    path:    " << storage.storagePath << "\n"
              << "    file:    " << storage.fileBytes << " bytes\n"
              << "    total:   " << storage.storageBytes << " bytes\n"
              << "    ratio:   " << std::setprecision(3) << storage.storageRatio << "x\n"
              << "    score:   " << storage.storageScore << " / 20\n\n";

    if (opt.repeats > 1) {
        std::cout << "  Repeat statistics (ms):\n";
        for (int g = 0; g < 6; ++g) {
            const auto& agg = aggregates[g];
            std::cout << "    " << agg.bestResult.axis << "_" << agg.bestResult.mode
                      << ": min=" << agg.minMs
                      << " mean=" << agg.meanMs
                      << " median=" << agg.medianMs
                      << " max=" << agg.maxMs << "\n";
        }
        std::cout << "\n";
    }

    if (baselineMs > 0) {
        double perfScore = (baselineMs / tComposite) * 60.0;
        std::cout << "  Performance (60pts):\n"
                  << "    Baseline:  " << std::setprecision(4) << baselineMs / 1000.0 << "s\n"
                  << "    Score:     (" << baselineMs / 1000.0 << " / " << std::setprecision(4) << tComposite / 1000.0
                  << ") × 60 = " << std::setprecision(1) << perfScore << " / 60\n\n"
                  << "  Total (excl. docs): " << perfScore + storage.storageScore << " / 80\n\n";
    } else {
        std::cout << "  Performance (60pts):\n"
                  << "    T_composite = " << std::setprecision(4) << tComposite / 1000.0 << "s\n"
                  << "    Score = (baseline / " << tComposite / 1000.0 << ") × 60\n"
                  << "    (use --baseline-ms or --baseline-file to compute score)\n\n";
    }

    std::cout << "  Reference (main-axis sequential × 3):\n"
              << "    Raw size:     " << storage.rawBytes << " bytes\n"
              << "    Each X slice: " << header.ny * header.nz * sizeof(float) << " bytes\n"
              << "    (Compare T_composite with your disk's sequential read of "
              << 3 * header.ny * header.nz * sizeof(float) << " bytes)\n\n";

    return 0;
}
