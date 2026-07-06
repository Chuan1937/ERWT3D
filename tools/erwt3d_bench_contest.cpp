#include "erwt3d/reader.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <random>
#include <chrono>
#include <algorithm>
#include <numeric>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <thread>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

static void printUsage(const char* prog) {
    std::cerr << "Usage: " << prog << " --input data.erwt3d --output-dir DIR [options]\n\n"
              << "Competition-standard benchmark (赛题2 评分标准)\n\n"
              << "  T_composite = (T_xr + T_yr + T_zr + T_xc + T_yc + T_zc) / 6\n"
              << "  Score = (baseline / T_composite) × 60\n\n"
              << "Options:\n"
              << "  --input PATH           ERWT3D file (required)\n"
              << "  --output-dir DIR       Output directory (required)\n"
              << "  --random-count N       Random slices per axis (default: 100)\n"
              << "  --continuous-count N   Continuous slices per axis (default: 10)\n"
              << "  --threads N|auto       Thread count (default: 1)\n"
              << "  --memory-limit-mb N    Memory limit (default: 2048)\n"
              << "  --cache-mb N           LRU cache size (default: 0)\n"
              << "  --io-backend pread|sb  I/O backend (default: sb)\n"
              << "  --sb-read-mode run-batch|leaf-index|hdd-read-window (default: hdd-read-window)\n"
              << "  --sb-task-order logical|file-offset (default: file-offset)\n"
              << "  --hdd-read-window-bytes N  HDD read window (0=auto, default: 0)\n"
              << "  --hdd-max-gap-bytes N      HDD max gap to merge (default: 0)\n"
              << "  --pin-threads          Pin threads to CPU cores\n"
              << "  --seed N               Random seed (default: 20260511)\n"
              << "  --dry-run              Print plan only, skip actual reads\n"
              << "  --baseline-ms N        Baseline T_composite for score calc\n"
              << "  --baseline-file PATH   Read baseline T_composite from CSV\n"
              << "  --repeats N            Repeat each group N times, take min (default: 1)\n";
}

struct GroupResult {
    std::string axis;
    std::string mode;
    int sliceCount;
    double groupTimeMs;
    double readTimeMs;
    double writeTimeMs;
    uint64_t outputBytesPerSlice;
    std::vector<double> perSliceTimes;
};

static double percentile(std::vector<double> sorted, double p) {
    if (sorted.empty()) return 0;
    size_t idx = static_cast<size_t>(p * sorted.size());
    if (idx >= sorted.size()) idx = sorted.size() - 1;
    return sorted[idx];
}

// Run one group: read all slices, write each to file, return total wall time
static bool runGroup(erwt3d::ERWT3DReader& reader,
                     erwt3d::SliceAxis axis, const std::string& axisName,
                     const std::vector<uint64_t>& indices, const std::string& mode,
                     const erwt3d::ERWT3DHeader& header,
                     int numThreads, size_t memoryLimitMB,
                     const std::string& outputDir,
                     GroupResult& result,
                     bool useBatch = false,
                     const erwt3d::HDDReadWindowConfig& wcfg = {}) {
    uint64_t sliceSize;
    switch (axis) {
        case erwt3d::SliceAxis::X: sliceSize = header.ny * header.nz; break;
        case erwt3d::SliceAxis::Y: sliceSize = header.nx * header.nz; break;
        case erwt3d::SliceAxis::Z: sliceSize = header.nx * header.ny; break;
    }
    uint64_t outBytes = sliceSize * sizeof(float);

    result.axis = axisName;
    result.mode = mode;
    result.sliceCount = static_cast<int>(indices.size());
    result.outputBytesPerSlice = outBytes;
    result.readTimeMs = 0;
    result.writeTimeMs = 0;
    result.perSliceTimes.clear();
    result.perSliceTimes.reserve(indices.size());

    // Pre-create output files BEFORE timing, with pre-allocation
    std::vector<int> preCreatedFDs(indices.size(), -1);
    for (size_t i = 0; i < indices.size(); ++i) {
        std::string outPath = outputDir + "/" + axisName + "_" + mode + "_" + std::to_string(i) + ".raw";
        int fd = open(outPath.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            std::cerr << "\nError: Cannot pre-create " << outPath << "\n";
            return false;
        }
        if (ftruncate(fd, static_cast<off_t>(outBytes)) != 0) {
            std::cerr << "\nError: Cannot pre-allocate " << outPath << "\n";
            close(fd);
            return false;
        }
        preCreatedFDs[i] = fd;
    }

    // Group wall-clock timing (includes read + write, same as competition)
    auto groupStart = std::chrono::high_resolution_clock::now();

    if (useBatch) {
        size_t totalSlices = indices.size();
        uint64_t sbBytes = static_cast<uint64_t>(header.super_x) * header.super_y * header.super_z * sizeof(float);
        uint64_t readWindow = wcfg.read_window_bytes > 0 ? wcfg.read_window_bytes : 128ULL * 1024 * 1024;
        size_t readBufPerThread = static_cast<size_t>(
            std::min<uint64_t>(memoryLimitMB * 1024ULL * 1024ULL / std::max(numThreads, 1),
                               std::max(readWindow, sbBytes * 4)));
        size_t totalReadBufMB = readBufPerThread * std::max(numThreads, 1) / (1024ULL * 1024ULL);
        size_t outputBudgetMB = (memoryLimitMB > totalReadBufMB + 64)
            ? memoryLimitMB - totalReadBufMB - 64
            : memoryLimitMB / 2;
        size_t maxBatch = (outputBudgetMB * 1024ULL * 1024ULL) / (outBytes + 1);
        if (maxBatch < 1) maxBatch = 1;
        size_t batchSize = std::min(totalSlices, maxBatch);

        double totalReadMs = 0, totalWriteMs = 0;

        for (size_t batchStart = 0; batchStart < totalSlices; batchStart += batchSize) {
            size_t batchEnd = std::min(batchStart + batchSize, totalSlices);
            size_t batchLen = batchEnd - batchStart;

            std::vector<erwt3d::ERWT3DReader::SliceBatchRequest> reqs;
            std::vector<std::vector<float>> buffers(batchLen);
            for (size_t i = 0; i < batchLen; ++i) {
                buffers[i].resize(sliceSize);
                reqs.push_back({axis, indices[batchStart + i], buffers[i].data()});
            }

            auto rStart = std::chrono::high_resolution_clock::now();
            if (!reader.readSlicesBatch(reqs, numThreads, memoryLimitMB, wcfg)) {
                std::cerr << "\nError: batch read failed for " << axisName << "\n";
                for (auto fd : preCreatedFDs) if (fd >= 0) close(fd);
                return false;
            }
            auto rEnd = std::chrono::high_resolution_clock::now();
            totalReadMs += std::chrono::duration<double, std::milli>(rEnd - rStart).count();

            for (size_t i = 0; i < batchLen; ++i) {
                auto wStart = std::chrono::high_resolution_clock::now();
                ssize_t written = pwrite(preCreatedFDs[batchStart + i], buffers[i].data(), outBytes, 0);
                auto wEnd = std::chrono::high_resolution_clock::now();
                if (written != static_cast<ssize_t>(outBytes)) {
                    std::cerr << "\nError: Write failed for " << axisName << "[" << (batchStart+i) << "]\n";
                    for (auto fd : preCreatedFDs) if (fd >= 0) close(fd);
                    return false;
                }
                double t = std::chrono::duration<double, std::milli>(wEnd - wStart).count();
                totalWriteMs += t;
                result.perSliceTimes.push_back(t);
            }
        }
        result.readTimeMs = totalReadMs;
        result.writeTimeMs = totalWriteMs;
    } else {
        std::vector<float> output(sliceSize);
        double totalReadMs = 0, totalWriteMs = 0;

        for (size_t i = 0; i < indices.size(); ++i) {
            auto rStart = std::chrono::high_resolution_clock::now();
            if (!reader.readSlice(axis, indices[i], output.data(), numThreads, memoryLimitMB)) {
                std::cerr << "\nError: readSlice failed for " << axisName << "[" << indices[i] << "]\n";
                for (auto fd : preCreatedFDs) if (fd >= 0) close(fd);
                return false;
            }
            auto rEnd = std::chrono::high_resolution_clock::now();
            totalReadMs += std::chrono::duration<double, std::milli>(rEnd - rStart).count();

            auto wStart = std::chrono::high_resolution_clock::now();
            ssize_t written = pwrite(preCreatedFDs[i], output.data(), outBytes, 0);
            auto wEnd = std::chrono::high_resolution_clock::now();
            if (written != static_cast<ssize_t>(outBytes)) {
                std::cerr << "\nError: Write failed for " << axisName << "[" << i << "]\n";
                for (auto fd : preCreatedFDs) if (fd >= 0) close(fd);
                return false;
            }
            double t = std::chrono::duration<double, std::milli>(wEnd - wStart).count();
            totalWriteMs += t;
            result.perSliceTimes.push_back(t);
        }
        result.readTimeMs = totalReadMs;
        result.writeTimeMs = totalWriteMs;
    }

    auto groupEnd = std::chrono::high_resolution_clock::now();
    result.groupTimeMs = std::chrono::duration<double, std::milli>(groupEnd - groupStart).count();

    // Close all pre-created file descriptors (after timing)
    for (auto fd : preCreatedFDs) {
        if (fd >= 0) close(fd);
    }

    return true;
}

int main(int argc, char* argv[]) {
    std::string inputPath, outputDir, baselineFile;
    int randomCount = 100, continuousCount = 10;
    int numThreads = 1;
    size_t memoryLimitMB = 2048, cacheMB = 0;
    uint32_t seed = 20260511;
    std::string ioBackendStr = "sb";
    std::string sbReadModeStr = "hdd-read-window", sbTaskOrderStr = "file-offset";
    uint64_t hddReadWindowBytes = 0, hddMaxGapBytes = 0;
    bool pinThreads = false, dryRun = false;
    bool useBatch = true;
    bool useMmap = false;
    bool hddMode = false;
    double baselineMsOverride = 0;
    int repeats = 1;

    for (int i = 1; i < argc; ++i) {
        auto next = [&]() -> const char* {
            if (i + 1 < argc) return argv[++i];
            std::cerr << "Error: " << argv[i] << " requires a value\n";
            std::exit(1);
            return nullptr;
        };
        if (std::strcmp(argv[i], "--input") == 0 || std::strcmp(argv[i], "-i") == 0) { inputPath = next(); }
        else if (std::strcmp(argv[i], "--output-dir") == 0 || std::strcmp(argv[i], "-o") == 0) { outputDir = next(); }
        else if (std::strcmp(argv[i], "--random-count") == 0) { randomCount = std::stoi(next()); }
        else if (std::strcmp(argv[i], "--continuous-count") == 0) { continuousCount = std::stoi(next()); }
        else if (std::strcmp(argv[i], "--threads") == 0 || std::strcmp(argv[i], "-t") == 0) {
            const char* v = next();
            if (std::strcmp(v, "auto") == 0) {
                unsigned hw = std::thread::hardware_concurrency();
                numThreads = static_cast<int>(std::min(std::max(1u, hw / 2), 8u));
            } else { numThreads = std::stoi(v); }
        }
        else if (std::strcmp(argv[i], "--memory-limit-mb") == 0 || std::strcmp(argv[i], "-m") == 0) { memoryLimitMB = std::stoul(next()); }
        else if (std::strcmp(argv[i], "--cache-mb") == 0) { cacheMB = std::stoul(next()); }
        else if (std::strcmp(argv[i], "--io-backend") == 0) { ioBackendStr = next(); }
        else if (std::strcmp(argv[i], "--sb-read-mode") == 0) { sbReadModeStr = next(); }
        else if (std::strcmp(argv[i], "--sb-task-order") == 0) { sbTaskOrderStr = next(); }
        else if (std::strcmp(argv[i], "--hdd-read-window-bytes") == 0) { hddReadWindowBytes = std::stoul(next()); }
        else if (std::strcmp(argv[i], "--hdd-max-gap-bytes") == 0) { hddMaxGapBytes = std::stoul(next()); }
        else if (std::strcmp(argv[i], "--pin-threads") == 0) { pinThreads = true; }
        else if (std::strcmp(argv[i], "--seed") == 0) { seed = std::stoul(next()); }
        else if (std::strcmp(argv[i], "--dry-run") == 0) { dryRun = true; }
        else if (std::strcmp(argv[i], "--batch") == 0) { useBatch = true; }
        else if (std::strcmp(argv[i], "--mmap") == 0) { useMmap = true; }
        else if (std::strcmp(argv[i], "--hdd") == 0) {
            hddMode = true;
            numThreads = 1;
            memoryLimitMB = 4096;
            ioBackendStr = "sb";
            sbReadModeStr = "hdd-read-window";
            sbTaskOrderStr = "file-offset";
            hddReadWindowBytes = 134217728;  // 128MB
            hddMaxGapBytes = 3145728;        // 3MB
            useBatch = true;
        }
        else if (std::strcmp(argv[i], "--baseline-ms") == 0) { baselineMsOverride = std::stod(next()); }
        else if (std::strcmp(argv[i], "--baseline-file") == 0) { baselineFile = next(); }
        else if (std::strcmp(argv[i], "--repeats") == 0) { repeats = std::stoi(next()); }
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]); return 0;
        }
        else { std::cerr << "Unknown option: " << argv[i] << "\n"; printUsage(argv[0]); return 1; }
    }

    if (inputPath.empty() || outputDir.empty()) {
        std::cerr << "Error: --input and --output-dir are required\n";
        printUsage(argv[0]); return 1;
    }

    { std::error_code ec; std::filesystem::create_directories(outputDir, ec);
      if (ec) { std::cerr << "Error: " << ec.message() << "\n"; return 1; } }

    erwt3d::ERWT3DReader reader(inputPath, cacheMB, useMmap);
    if (ioBackendStr == "sb" || ioBackendStr == "superblock")
        reader.setIOBackend(erwt3d::IOBackend::Superblock);
    if (sbReadModeStr == "run-batch") reader.setSBReadMode(erwt3d::SBReadMode::RunBatch);
    else if (sbReadModeStr == "leaf-index") reader.setSBReadMode(erwt3d::SBReadMode::LeafIndex);
    else if (sbReadModeStr == "hdd-read-window") reader.setSBReadMode(erwt3d::SBReadMode::HDDReadWindow);
    if (sbTaskOrderStr == "file-offset") reader.setSBTaskOrder(erwt3d::SBTaskOrder::FileOffset);
    if (hddReadWindowBytes > 0 || hddMaxGapBytes > 0) {
        reader.setHDDReadWindowConfig({hddReadWindowBytes, hddMaxGapBytes});
    }
    reader.setPinThreads(pinThreads);

    const auto& header = reader.getHeader();

    // Storage check
    uint64_t rawBytes = erwt3d::getRawSize(header);
    struct stat fileStat;
    uint64_t fileBytes = 0;
    if (stat(inputPath.c_str(), &fileStat) == 0) fileBytes = fileStat.st_size;
    double storageRatio = static_cast<double>(fileBytes) / rawBytes;
    int storageScore = 20;
    if (storageRatio > 1.5) {
        double over = storageRatio - 1.5;
        int penalty = static_cast<int>(std::ceil(over / 0.1));
        storageScore = std::max(0, 20 - penalty);
    }

    std::cout << "============================================================\n"
              << "  ERWT3D Competition Benchmark (赛题2 评分标准)\n"
              << "============================================================\n"
              << "  File:      " << inputPath << "\n"
              << "  Dims:      " << header.nx << " x " << header.ny << " x " << header.nz << "\n"
              << "  Random:    " << randomCount << " slices/axis\n"
              << "  Continuous:" << continuousCount << " slices/axis\n"
              << "  Threads:   " << numThreads << "\n"
              << "  MemLimit:  " << memoryLimitMB << " MB\n"
              << "  Cache:     " << cacheMB << " MB\n"
              << "  Backend:   " << ioBackendStr << "\n"
              << "  ReadMode:  " << sbReadModeStr << "\n"
              << "  TaskOrd:   " << sbTaskOrderStr << "\n"
              << "  Repeats:   " << repeats << "\n"
              << "  Storage:   " << fileBytes << " bytes (" << std::fixed << std::setprecision(3)
              << storageRatio << "x) -> " << storageScore << "/20 pts\n"
              << "============================================================\n\n";

    // Generate indices
    std::mt19937 rng(seed);
    std::uniform_int_distribution<uint64_t> distX(0, header.nx - 1);
    std::uniform_int_distribution<uint64_t> distY(0, header.ny - 1);
    std::uniform_int_distribution<uint64_t> distZ(0, header.nz - 1);

    std::vector<uint64_t> randomX(randomCount), randomY(randomCount), randomZ(randomCount);
    for (int i = 0; i < randomCount; ++i) {
        randomX[i] = distX(rng); randomY[i] = distY(rng); randomZ[i] = distZ(rng);
    }

    auto safeStart = [](uint64_t dim, int cnt) -> uint64_t {
        if (static_cast<uint64_t>(cnt) >= dim) return 0;
        return dim / 2 - cnt / 2;
    };
    int countX = std::min(continuousCount, static_cast<int>(header.nx));
    int countY = std::min(continuousCount, static_cast<int>(header.ny));
    int countZ = std::min(continuousCount, static_cast<int>(header.nz));
    std::vector<uint64_t> continuousX(countX), continuousY(countY), continuousZ(countZ);
    uint64_t sx = safeStart(header.nx, countX);
    uint64_t sy = safeStart(header.ny, countY);
    uint64_t sz = safeStart(header.nz, countZ);
    for (int i = 0; i < countX; ++i) continuousX[i] = sx + i;
    for (int i = 0; i < countY; ++i) continuousY[i] = sy + i;
    for (int i = 0; i < countZ; ++i) continuousZ[i] = sz + i;

    if (dryRun) {
        std::cout << "[DRY RUN] Would benchmark 6 groups:\n"
                  << "  X random:     " << randomCount << " slices, dim=" << header.ny << "x" << header.nz << "\n"
                  << "  Y random:     " << randomCount << " slices, dim=" << header.nx << "x" << header.nz << "\n"
                  << "  Z random:     " << randomCount << " slices, dim=" << header.nx << "x" << header.ny << "\n"
                  << "  X continuous:  " << countX << " slices starting at " << sx << "\n"
                  << "  Y continuous:  " << countY << " slices starting at " << sy << "\n"
                  << "  Z continuous:  " << countZ << " slices starting at " << sz << "\n"
                  << "  Storage:  " << storageRatio << "x -> " << storageScore << "/20\n";
        return 0;
    }

    // Define the 6 benchmark groups
    struct GroupSpec {
        erwt3d::SliceAxis axis;
        std::string axisName, mode;
        const std::vector<uint64_t>* indices;
    };
    std::vector<GroupSpec> groups = {
        {erwt3d::SliceAxis::X, "x", "random",     &randomX},
        {erwt3d::SliceAxis::Y, "y", "random",     &randomY},
        {erwt3d::SliceAxis::Z, "z", "random",     &randomZ},
        {erwt3d::SliceAxis::X, "x", "continuous", &continuousX},
        {erwt3d::SliceAxis::Y, "y", "continuous", &continuousY},
        {erwt3d::SliceAxis::Z, "z", "continuous", &continuousZ},
    };

    // Run all 6 groups, with optional repeats (take min per group)
    std::vector<GroupResult> results(6);
    double groupTimes[6];

    std::cout << "Running 6 benchmark groups...\n\n";

    for (int g = 0; g < 6; ++g) {
        const auto& spec = groups[g];
        double bestGroupMs = 1e18;
        GroupResult bestResult;

        for (int rep = 0; rep < repeats; ++rep) {
            std::string repDir = outputDir;
            if (repeats > 1) repDir = outputDir + "/rep" + std::to_string(rep);
            std::error_code ec;
            std::filesystem::create_directories(repDir, ec);

            GroupResult gr;
            std::cout << "  [" << (g+1) << "/6] " << spec.axisName << " " << spec.mode
                      << " (" << spec.indices->size() << " slices)" << std::flush;
            if (repeats > 1) std::cout << " rep=" << rep << std::flush;
            std::cout << "..." << std::flush;

            erwt3d::HDDReadWindowConfig wcfg{hddReadWindowBytes, hddMaxGapBytes};
            if (!runGroup(reader, spec.axis, spec.axisName, *spec.indices, spec.mode,
                          header, numThreads, memoryLimitMB, repDir, gr, useBatch, wcfg)) {
                return 1;
            }

            std::cout << " " << std::fixed << std::setprecision(4) << gr.groupTimeMs / 1000.0 << "s"
                      << " (r=" << std::setprecision(3) << gr.readTimeMs / 1000.0
                      << "s w=" << gr.writeTimeMs / 1000.0 << "s)\n";

            if (gr.groupTimeMs < bestGroupMs) {
                bestGroupMs = gr.groupTimeMs;
                bestResult = gr;
            }
        }

        results[g] = bestResult;
        groupTimes[g] = bestGroupMs;
    }

    // Compute T_composite = sum of 6 group times / 6
    double totalAllGroups = 0;
    for (int g = 0; g < 6; ++g) totalAllGroups += groupTimes[g];
    double tComposite = totalAllGroups / 6.0;

    // Also compute per-axis averages for diagnostics
    double avgX = (groupTimes[0] + groupTimes[3]) / 2.0;
    double avgY = (groupTimes[1] + groupTimes[4]) / 2.0;
    double avgZ = (groupTimes[2] + groupTimes[5]) / 2.0;

    // Determine baseline
    double baselineMs = baselineMsOverride;
    if (baselineMs <= 0 && !baselineFile.empty()) {
        std::ifstream bf(baselineFile);
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

    // Write per-slice detail CSV
    std::string detailPath = outputDir + "/contest_detail.csv";
    {
        std::ofstream df(detailPath);
        df << "group,axis,mode,iteration,slice_index,time_ms,output_bytes\n";
        for (int g = 0; g < 6; ++g) {
            const auto& r = results[g];
            const auto& idxVec = (r.mode == "random") ?
                (r.axis == "x" ? randomX : r.axis == "y" ? randomY : randomZ) :
                (r.axis == "x" ? continuousX : r.axis == "y" ? continuousY : continuousZ);
            for (size_t i = 0; i < r.perSliceTimes.size(); ++i) {
                df << g << "," << r.axis << "," << r.mode << "," << i << ","
                   << idxVec[i] << ","
                   << std::fixed << std::setprecision(3) << r.perSliceTimes[i]
                   << "," << r.outputBytesPerSlice << "\n";
            }
        }
    }

    // Write group summary CSV
    std::string summaryPath = outputDir + "/contest_summary.csv";
    {
        std::ofstream sf(summaryPath);
        sf << "group,axis,mode,slice_count,group_time_ms,read_time_ms,write_time_ms,avg_per_slice_ms,output_bytes_per_slice\n";
        for (int g = 0; g < 6; ++g) {
            const auto& r = results[g];
            sf << g << "," << r.axis << "," << r.mode << "," << r.sliceCount << ","
               << std::fixed << std::setprecision(3) << r.groupTimeMs << ","
               << r.readTimeMs << "," << r.writeTimeMs << ","
               << r.groupTimeMs / r.sliceCount << "," << r.outputBytesPerSlice << "\n";
        }
    }

    // Write score CSV
    std::string scorePath = outputDir + "/contest_score.csv";
    {
        std::ofstream sc(scorePath);
        sc << "metric,value\n"
           << "input_file," << inputPath << "\n"
           << "dimensions," << header.nx << "x" << header.ny << "x" << header.nz << "\n"
           << "random_count," << randomCount << "\n"
           << "continuous_count," << continuousCount << "\n"
           << "threads," << numThreads << "\n"
           << "memory_limit_mb," << memoryLimitMB << "\n"
           << "io_backend," << ioBackendStr << "\n"
           << "sb_read_mode," << sbReadModeStr << "\n"
           << std::fixed << std::setprecision(3)
           << "T_x_random_ms," << groupTimes[0] << "\n"
           << "T_y_random_ms," << groupTimes[1] << "\n"
            << "T_z_random_ms," << groupTimes[2] << "\n"
            << "T_x_continuous_ms," << groupTimes[3] << "\n"
            << "T_y_continuous_ms," << groupTimes[4] << "\n"
            << "T_z_continuous_ms," << groupTimes[5] << "\n"
            << "read_x_random_ms," << results[0].readTimeMs << "\n"
            << "write_x_random_ms," << results[0].writeTimeMs << "\n"
            << "read_y_random_ms," << results[1].readTimeMs << "\n"
            << "write_y_random_ms," << results[1].writeTimeMs << "\n"
            << "read_z_random_ms," << results[2].readTimeMs << "\n"
            << "write_z_random_ms," << results[2].writeTimeMs << "\n"
            << "total_all_groups_ms," << totalAllGroups << "\n"
           << "T_composite_ms," << tComposite << "\n"
           << "avg_X_ms," << avgX << "\n"
           << "avg_Y_ms," << avgY << "\n"
           << "avg_Z_ms," << avgZ << "\n"
           << "file_bytes," << fileBytes << "\n"
           << "raw_bytes," << rawBytes << "\n"
           << "storage_ratio," << storageRatio << "\n"
           << "storage_score," << storageScore << "\n";
    }

    // Print final summary
    std::cout << "\n============================================================\n"
              << "  COMPETITION SCORE (赛题2 评分标准)\n"
              << "============================================================\n\n"
               << "  6 Group Times (wall-clock, read + write):\n"
              << "    [1] X random:      " << std::setw(8) << std::fixed << std::setprecision(4)
              << groupTimes[0] / 1000.0 << "s  (" << randomCount << " slices)  r=" << std::setprecision(3)
              << results[0].readTimeMs / 1000.0 << "s w=" << results[0].writeTimeMs / 1000.0 << "s\n"
              << "    [2] Y random:      " << std::setw(8) << groupTimes[1] / 1000.0
              << "s  r=" << results[1].readTimeMs / 1000.0 << "s w=" << results[1].writeTimeMs / 1000.0 << "s\n"
              << "    [3] Z random:      " << std::setw(8) << groupTimes[2] / 1000.0
              << "s  r=" << results[2].readTimeMs / 1000.0 << "s w=" << results[2].writeTimeMs / 1000.0 << "s\n"
              << "    [4] X continuous:  " << std::setw(8) << groupTimes[3] / 1000.0 << "s  (" << countX << " slices)\n"
              << "    [5] Y continuous:  " << std::setw(8) << groupTimes[4] / 1000.0 << "s\n"
              << "    [6] Z continuous:  " << std::setw(8) << groupTimes[5] / 1000.0 << "s\n"
              << "    ----------------------------------------\n"
              << "    Total:             " << std::setw(8) << totalAllGroups / 1000.0 << "s\n"
              << "    T_composite = total/6 = " << tComposite / 1000.0 << "s\n\n"
              << "  Per-axis diagnostics:\n"
              << "    avg X = (random+cont)/2 = " << avgX / 1000.0 << "s\n"
              << "    avg Y = " << avgY / 1000.0 << "s\n"
              << "    avg Z = " << avgZ / 1000.0 << "s\n\n"
              << "  Storage (20pts):\n"
              << "    Ratio:   " << std::setprecision(3) << storageRatio << "x\n"
              << "    Score:   " << storageScore << " / 20\n\n";

    if (baselineMs > 0) {
        double perfScore = (baselineMs / tComposite) * 60.0;
        std::cout << "  Performance (60pts):\n"
                  << "    Baseline:  " << std::setprecision(4) << baselineMs / 1000.0 << "s\n"
                  << "    Score:     (" << baselineMs / 1000.0 << " / " << std::setprecision(4) << tComposite / 1000.0
                  << ") × 60 = " << std::setprecision(1) << perfScore << " / 60\n\n"
                  << "  Total (excl. docs): " << perfScore + storageScore << " / 80\n\n";
    } else {
        std::cout << "  Performance (60pts):\n"
                  << "    T_composite = " << std::setprecision(4) << tComposite / 1000.0 << "s\n"
                  << "    Score = (baseline / " << tComposite / 1000.0 << ") × 60\n"
                  << "    (use --baseline-ms or --baseline-file to compute score)\n\n";
    }

    // Reference: main-axis sequential read × 3
    // For row-major (z*ny+y)*nx+x, the "main axis" is X (nx is fastest-varying)
    // Sequential read of entire data = rawBytes / disk_bandwidth
    // Baseline estimate ≈ 3 × sequential read (3 axes, each needing ~1 full pass)
    // This is a rough reference only
    std::cout << "  Reference (main-axis sequential × 3):\n"
              << "    Raw size:     " << rawBytes << " bytes (" << (rawBytes / (1024.0*1024*1024)) << " GiB)\n"
              << "    Estimate:     T_composite ≈ 3 × (raw_size / disk_bandwidth)\n"
              << "                 e.g. HDD 300MB/s → ~" << std::fixed << std::setprecision(1)
              << (3.0 * rawBytes / (300.0 * 1024 * 1024)) << "s\n\n";

    std::cout << "  Output:\n"
              << "    Score:   " << scorePath << "\n"
              << "    Summary: " << summaryPath << "\n"
              << "    Detail:  " << detailPath << "\n"
              << "============================================================\n";

    return 0;
}
