#include "erwt3d/rzfp_reader.hpp"
#include "erwt3d/rzfp_format.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

struct GroupResult {
    std::string axis;
    std::string mode;
    int sliceCount = 0;
    double groupTimeMs = 0.0;
    double readTimeMs = 0.0;
    double writeTimeMs = 0.0;
    uint64_t outputBytesPerSlice = 0;
    erwt3d::RzfpReadProfile profile;
};

static bool runGroup(erwt3d::RzfpReader& reader,
                     const erwt3d::RzfpFileHeader& header,
                     erwt3d::SliceAxis axis, const std::string& axisName,
                     const std::vector<uint64_t>& indices, const std::string& mode,
                     int numThreads, size_t memoryLimitMB,
                     const std::string& outputDir,
                     const erwt3d::RzfpReaderConfig& rzcfg,
                     GroupResult& result) {
    uint64_t sliceSize;
    switch (axis) {
        case erwt3d::SliceAxis::X: sliceSize = header.ny * header.nz; break;
        case erwt3d::SliceAxis::Y: sliceSize = header.nx * header.nz; break;
        case erwt3d::SliceAxis::Z: sliceSize = header.nx * header.ny; break;
    }
    const uint64_t outBytes = sliceSize * sizeof(float);

    result.axis = axisName;
    result.mode = mode;
    result.sliceCount = static_cast<int>(indices.size());
    result.outputBytesPerSlice = outBytes;

    // Pre-create output files
    std::vector<int> fds(indices.size(), -1);
    for (size_t i = 0; i < indices.size(); ++i) {
        std::string outPath = outputDir + "/" + axisName + "_" + mode + "_" + std::to_string(i) + ".raw";
        int fd = open(outPath.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            std::cerr << "\nError: Cannot pre-create " << outPath << "\n";
            return false;
        }
        if (posix_fallocate(fd, 0, static_cast<off_t>(outBytes)) != 0) {
            if (ftruncate(fd, static_cast<off_t>(outBytes)) != 0) {
                std::cerr << "\nError: Cannot pre-allocate " << outPath << "\n";
                close(fd);
                return false;
            }
        }
        fds[i] = fd;
    }

    // Compute batch size from memory budget
    const size_t totalOutputBytes = indices.size() * outBytes;
    const size_t memoryLimitBytes = memoryLimitMB * 1024 * 1024;
    size_t maxBatch;
    if (totalOutputBytes + 128ULL * 1024 * 1024 <= memoryLimitBytes) {
        maxBatch = indices.size();
    } else {
        size_t totalReadBufMB = std::max<size_t>(1, memoryLimitMB / 2);
        size_t outputBudgetMB = (memoryLimitMB > totalReadBufMB + 64)
                                    ? memoryLimitMB - totalReadBufMB - 64
                                    : memoryLimitMB / 2;
        maxBatch = (outputBudgetMB * 1024 * 1024) / (outBytes + 1);
        if (maxBatch < 1) maxBatch = 1;
    }

    auto groupStart = std::chrono::high_resolution_clock::now();
    double totalReadMs = 0.0;
    double totalWriteMs = 0.0;

    erwt3d::RzfpReadProfile accumulated;

    for (size_t batchStart = 0; batchStart < indices.size(); batchStart += maxBatch) {
        size_t batchEnd = std::min(batchStart + maxBatch, indices.size());
        size_t batchLen = batchEnd - batchStart;

        std::vector<std::vector<float>> buffers(batchLen);
        std::vector<erwt3d::RzfpReader::SliceBatchRequest> reqs;
        for (size_t i = 0; i < batchLen; ++i) {
            buffers[i].resize(sliceSize);
            reqs.push_back({axis, indices[batchStart + i], buffers[i].data()});
        }

        erwt3d::RzfpReadProfile batchProfile;
        erwt3d::RzfpReaderConfig batchCfg = rzcfg;
        batchCfg.profile = &batchProfile;

        auto rStart = std::chrono::high_resolution_clock::now();
        if (!reader.readSlicesBatch(reqs, batchCfg)) {
            std::cerr << "\nError: batch read failed for " << axisName << "\n";
            for (int fd : fds) if (fd >= 0) close(fd);
            return false;
        }
        auto rEnd = std::chrono::high_resolution_clock::now();
        totalReadMs += std::chrono::duration<double, std::milli>(rEnd - rStart).count();

        accumulated.unique_superblocks += batchProfile.unique_superblocks;
        accumulated.unique_leaves += batchProfile.unique_leaves;
        accumulated.requested_record_bytes += batchProfile.requested_record_bytes;
        accumulated.actual_read_bytes += batchProfile.actual_read_bytes;
        accumulated.pread_calls += batchProfile.pread_calls;
        accumulated.io_time_ms += batchProfile.io_time_ms;
        accumulated.decode_time_ms += batchProfile.decode_time_ms;
        accumulated.scatter_time_ms += batchProfile.scatter_time_ms;
        accumulated.plan_time_ms += batchProfile.plan_time_ms;
        accumulated.prefix_time_ms += batchProfile.prefix_time_ms;

        for (size_t i = 0; i < batchLen; ++i) {
            auto wStart = std::chrono::high_resolution_clock::now();
            ssize_t written = pwrite(fds[batchStart + i], buffers[i].data(), outBytes, 0);
            auto wEnd = std::chrono::high_resolution_clock::now();
            if (written != static_cast<ssize_t>(outBytes)) {
                std::cerr << "\nError: Write failed for " << axisName << "[" << (batchStart + i) << "]\n";
                for (int fd : fds) if (fd >= 0) close(fd);
                return false;
            }
            totalWriteMs += std::chrono::duration<double, std::milli>(wEnd - wStart).count();
        }
    }

    for (int fd : fds) if (fd >= 0) close(fd);

    auto groupEnd = std::chrono::high_resolution_clock::now();
    result.groupTimeMs = std::chrono::duration<double, std::milli>(groupEnd - groupStart).count();
    result.readTimeMs = totalReadMs;
    result.writeTimeMs = totalWriteMs;
    result.profile = accumulated;
    return true;
}

static void printUsage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " --input PATH --output-dir DIR [options]\n\n"
        << "Options:\n"
        << "  --random-count N       Random slices per axis (default: 100)\n"
        << "  --continuous-count N   Continuous slices per axis (default: 10)\n"
        << "  --threads N            Thread count (default: 1)\n"
        << "  --decode-threads N     RZFP decode threads (default: 1)\n"
        << "  --memory-limit-mb N    Memory limit (default: 4096)\n"
        << "  --read-window-bytes N  HDD read window (default: 16777216)\n"
        << "  --max-gap-bytes N      HDD max gap (default: 65536)\n"
        << "  --read-strategy STR    auto|selective|whole|fullscan (default: auto)\n"
        << "  --seed N               Random seed (default: 20260511)\n"
        << "  --hdd                  Enable HDD mode defaults\n";
}

int main(int argc, char* argv[]) {
    std::string inputPath, outputDir;
    int randomCount = 100;
    int continuousCount = 10;
    int numThreads = 1;
    int decodeThreads = 8;
    size_t memoryLimitMB = 4096;
    uint64_t readWindowBytes = 512ULL * 1024 * 1024;
    uint64_t maxGapBytes = 8ULL * 1024 * 1024;
    uint32_t seed = 20260511;
    std::string strategyStr = "auto";
    bool hddMode = false;

    for (int i = 1; i < argc; ++i) {
        auto next = [&]() -> const char* {
            if (i + 1 < argc) return argv[++i];
            std::cerr << "Error: " << argv[i] << " requires a value\n";
            std::exit(1);
            return nullptr;
        };
        if (std::strcmp(argv[i], "--input") == 0 || std::strcmp(argv[i], "-i") == 0) inputPath = next();
        else if (std::strcmp(argv[i], "--output-dir") == 0 || std::strcmp(argv[i], "-o") == 0) outputDir = next();
        else if (std::strcmp(argv[i], "--random-count") == 0) randomCount = std::stoi(next());
        else if (std::strcmp(argv[i], "--continuous-count") == 0) continuousCount = std::stoi(next());
        else if (std::strcmp(argv[i], "--threads") == 0 || std::strcmp(argv[i], "-t") == 0) numThreads = std::stoi(next());
        else if (std::strcmp(argv[i], "--decode-threads") == 0) decodeThreads = std::stoi(next());
        else if (std::strcmp(argv[i], "--memory-limit-mb") == 0 || std::strcmp(argv[i], "-m") == 0) memoryLimitMB = std::stoul(next());
        else if (std::strcmp(argv[i], "--read-strategy") == 0) strategyStr = next();
        else if (std::strcmp(argv[i], "--read-window-bytes") == 0) readWindowBytes = std::stoull(next());
        else if (std::strcmp(argv[i], "--max-gap-bytes") == 0) maxGapBytes = std::stoull(next());
        else if (std::strcmp(argv[i], "--seed") == 0) seed = static_cast<uint32_t>(std::stoul(next()));
        else if (std::strcmp(argv[i], "--hdd") == 0) {
            hddMode = true;
            numThreads = 1;
            decodeThreads = 8;
            memoryLimitMB = 4096;
            readWindowBytes = 512ULL * 1024 * 1024;
            maxGapBytes = 8ULL * 1024 * 1024;
        }
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]); return 0;
        }
        else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            printUsage(argv[0]); return 1;
        }
    }

    if (inputPath.empty() || outputDir.empty()) {
        std::cerr << "Error: --input and --output-dir are required\n";
        printUsage(argv[0]); return 1;
    }

    erwt3d::RzfpReadStrategy strategy = erwt3d::RzfpReadStrategy::Auto;
    if (strategyStr == "auto") strategy = erwt3d::RzfpReadStrategy::Auto;
    else if (strategyStr == "selective") strategy = erwt3d::RzfpReadStrategy::SelectiveLeaf;
    else if (strategyStr == "whole") strategy = erwt3d::RzfpReadStrategy::WholeSuperblock;
    else if (strategyStr == "fullscan") strategy = erwt3d::RzfpReadStrategy::FullPayloadScan;
    else {
        std::cerr << "Error: unknown --read-strategy: " << strategyStr << "\n";
        printUsage(argv[0]); return 1;
    }

    fs::create_directories(outputDir);

    erwt3d::RzfpReader reader(inputPath);
    if (!reader.ok()) {
        std::cerr << "Error: cannot open RZFP file: " << inputPath << std::endl;
        return 1;
    }
    const auto& header = reader.header();

    struct stat st;
    uint64_t fileBytes = 0;
    if (stat(inputPath.c_str(), &st) == 0) fileBytes = st.st_size;
    const std::string sidecarPath = inputPath + ".xp";
    if (stat(sidecarPath.c_str(), &st) == 0) fileBytes += st.st_size;
    const uint64_t rawBytes = erwt3d::rzfpRawSize(header);
    const double storageRatio = static_cast<double>(fileBytes) / static_cast<double>(rawBytes);

    std::mt19937 rng(seed);
    std::uniform_int_distribution<uint64_t> distX(0, header.nx - 1);
    std::uniform_int_distribution<uint64_t> distY(0, header.ny - 1);
    std::uniform_int_distribution<uint64_t> distZ(0, header.nz - 1);

    std::vector<uint64_t> randomX(randomCount), randomY(randomCount), randomZ(randomCount);
    for (int i = 0; i < randomCount; ++i) {
        randomX[i] = distX(rng);
        randomY[i] = distY(rng);
        randomZ[i] = distZ(rng);
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

    struct GroupSpec {
        erwt3d::SliceAxis axis;
        std::string axisName, mode;
        const std::vector<uint64_t>* indices;
    };
    std::vector<GroupSpec> groups = {
        {erwt3d::SliceAxis::X, "x", "random", &randomX},
        {erwt3d::SliceAxis::Y, "y", "random", &randomY},
        {erwt3d::SliceAxis::Z, "z", "random", &randomZ},
        {erwt3d::SliceAxis::X, "x", "continuous", &continuousX},
        {erwt3d::SliceAxis::Y, "y", "continuous", &continuousY},
        {erwt3d::SliceAxis::Z, "z", "continuous", &continuousZ},
    };

    erwt3d::RzfpReaderConfig rzcfg;
    rzcfg.hdd = erwt3d::HDDReadWindowConfig{readWindowBytes, maxGapBytes};
    rzcfg.strategy = strategy;
    rzcfg.decode_threads = decodeThreads;

    std::vector<GroupResult> results(6);
    double groupTimes[6];

    std::cout << "============================================================\n"
              << "  RZFP Competition Benchmark\n"
              << "============================================================\n"
              << "  File:      " << inputPath << "\n"
              << "  Dims:      " << header.nx << " x " << header.ny << " x " << header.nz << "\n"
              << "  Random:    " << randomCount << " slices/axis\n"
              << "  Continuous:" << continuousCount << " slices/axis\n"
              << "  Threads:   " << numThreads << "\n"
              << "  DecodeThr: " << decodeThreads << "\n"
              << "  Strategy:  " << strategyStr << "\n"
              << "  MemLimit:  " << memoryLimitMB << " MB\n"
              << "  HDD mode:  " << (hddMode ? "true" : "false") << "\n"
              << "  Window:    " << readWindowBytes << " bytes\n"
              << "  MaxGap:    " << maxGapBytes << " bytes\n"
              << "  Storage:   " << fileBytes << " bytes (ratio "
              << std::fixed << std::setprecision(3) << storageRatio << "x)\n"
              << "============================================================\n\n";

    for (int g = 0; g < 6; ++g) {
        const auto& spec = groups[g];
        std::cout << "  [" << (g + 1) << "/6] " << spec.axisName << " " << spec.mode
                  << " (" << spec.indices->size() << " slices)..." << std::flush;

        GroupResult gr;
        if (!runGroup(reader, header, spec.axis, spec.axisName, *spec.indices, spec.mode,
                      numThreads, memoryLimitMB, outputDir, rzcfg, gr)) {
            return 1;
        }

        std::cout << " " << std::fixed << std::setprecision(4) << gr.groupTimeMs / 1000.0 << "s"
                  << " (r=" << std::setprecision(3) << gr.readTimeMs / 1000.0
                  << "s w=" << gr.writeTimeMs / 1000.0 << "s)\n";
        const auto& p = gr.profile;
        std::cout << "       profile: sbs=" << p.unique_superblocks
                  << " leaves=" << p.unique_leaves
                  << " req=" << p.requested_record_bytes
                  << " actual=" << p.actual_read_bytes
                  << " amp=" << std::fixed << std::setprecision(2) << p.readAmplification()
                  << " preads=" << p.pread_calls
                  << " io=" << std::setprecision(1) << p.io_time_ms / 1000.0
                  << "s dec=" << p.decode_time_ms / 1000.0
                  << "s scat=" << p.scatter_time_ms / 1000.0 << "s\n";

        results[g] = gr;
        groupTimes[g] = gr.groupTimeMs;
    }

    double totalAll = 0.0;
    for (int g = 0; g < 6; ++g) totalAll += groupTimes[g];
    double tComposite = totalAll / 6.0;

    std::string summaryPath = outputDir + "/rzfp_contest_summary.csv";
    {
        std::ofstream sf(summaryPath);
        sf << "group,axis,mode,slice_count,group_time_ms,read_time_ms,write_time_ms,output_bytes_per_slice,"
              "unique_sbs,unique_leaves,requested_bytes,actual_bytes,read_amp,preads,io_ms,decode_ms,scatter_ms\n";
        for (int g = 0; g < 6; ++g) {
            const auto& r = results[g];
            const auto& p = r.profile;
            sf << g << "," << r.axis << "," << r.mode << "," << r.sliceCount << ","
               << std::fixed << std::setprecision(3) << r.groupTimeMs << ","
               << r.readTimeMs << "," << r.writeTimeMs << ","
               << r.outputBytesPerSlice << ","
               << p.unique_superblocks << "," << p.unique_leaves << ","
               << p.requested_record_bytes << "," << p.actual_read_bytes << ","
               << std::setprecision(3) << p.readAmplification() << ","
               << p.pread_calls << ","
               << std::setprecision(3) << p.io_time_ms << ","
               << p.decode_time_ms << "," << p.scatter_time_ms << "\n";
        }
    }

    std::string scorePath = outputDir + "/rzfp_contest_score.csv";
    {
        std::ofstream sc(scorePath);
        sc << "metric,value\n"
           << "input_file," << inputPath << "\n"
           << "dimensions," << header.nx << "x" << header.ny << "x" << header.nz << "\n"
           << "random_count," << randomCount << "\n"
           << "continuous_count," << continuousCount << "\n"
           << "threads," << numThreads << "\n"
           << "decode_threads," << decodeThreads << "\n"
           << "strategy," << strategyStr << "\n"
           << "memory_limit_mb," << memoryLimitMB << "\n"
           << "read_window_bytes," << readWindowBytes << "\n"
           << "max_gap_bytes," << maxGapBytes << "\n"
           << std::fixed << std::setprecision(3)
           << "T_x_random_ms," << groupTimes[0] << "\n"
           << "T_y_random_ms," << groupTimes[1] << "\n"
           << "T_z_random_ms," << groupTimes[2] << "\n"
           << "T_x_continuous_ms," << groupTimes[3] << "\n"
           << "T_y_continuous_ms," << groupTimes[4] << "\n"
           << "T_z_continuous_ms," << groupTimes[5] << "\n"
           << "total_all_groups_ms," << totalAll << "\n"
           << "T_composite_ms," << tComposite << "\n"
           << "file_bytes," << fileBytes << "\n"
           << "raw_bytes," << rawBytes << "\n"
           << "storage_ratio," << storageRatio << "\n";
    }

    std::cout << "\n============================================================\n"
              << "  RZFP COMPETITION SCORE\n"
              << "============================================================\n\n"
              << "  X random:      " << std::setw(8) << std::fixed << std::setprecision(4)
              << groupTimes[0] / 1000.0 << "s\n"
              << "  Y random:      " << std::setw(8) << groupTimes[1] / 1000.0 << "s\n"
              << "  Z random:      " << std::setw(8) << groupTimes[2] / 1000.0 << "s\n"
              << "  X continuous:  " << std::setw(8) << groupTimes[3] / 1000.0 << "s\n"
              << "  Y continuous:  " << std::setw(8) << groupTimes[4] / 1000.0 << "s\n"
              << "  Z continuous:  " << std::setw(8) << groupTimes[5] / 1000.0 << "s\n"
              << "  ----------------------------------------\n"
              << "  Total:         " << std::setw(8) << totalAll / 1000.0 << "s\n"
              << "  T_composite:   " << std::setw(8) << tComposite / 1000.0 << "s\n\n"
              << "  Storage ratio: " << std::setprecision(3) << storageRatio << "x\n\n"
              << "  Output:\n"
              << "    Summary: " << summaryPath << "\n"
              << "    Score:   " << scorePath << "\n"
              << "============================================================\n";

    return 0;
}
