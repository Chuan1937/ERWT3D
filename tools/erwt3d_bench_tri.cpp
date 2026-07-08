#include "erwt3d/tri_reader.hpp"
#include "erwt3d/tri_format.hpp"
#include "erwt3d/sb_hdd.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <random>
#include <algorithm>
#include <numeric>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <filesystem>
#include <iomanip>
#include <thread>

struct GroupResult {
    std::string axis;
    std::string mode;
    double groupTimeMs = 0;
    double readTimeMs = 0;
    double writeTimeMs = 0;
    int sliceCount = 0;
    uint64_t outputBytesPerSlice = 0;
    std::vector<double> perSliceTimes;
};

bool runGroup(erwt3d::TriReader& reader,
              erwt3d::TriSliceAxis axis, const std::string& axisName,
              const std::vector<uint64_t>& indices, const std::string& mode,
              uint64_t nx, uint64_t ny, uint64_t nz,
              int numThreads, size_t memoryLimitMB,
              const std::string& outputDir, GroupResult& result,
              bool hddMode) {
    uint64_t outDim1, outDim2;
    if (axis == erwt3d::TriSliceAxis::X) { outDim1 = ny; outDim2 = nz; }
    else if (axis == erwt3d::TriSliceAxis::Y) { outDim1 = nx; outDim2 = nz; }
    else { outDim1 = nx; outDim2 = ny; }

    uint64_t sliceSize = outDim1 * outDim2;
    uint64_t outBytes = sliceSize * sizeof(float);

    result.axis = axisName;
    result.mode = mode;
    result.sliceCount = (int)indices.size();
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
        if (ftruncate(fd, (off_t)outBytes) != 0) {
            std::cerr << "\nError: Cannot pre-allocate " << outPath << "\n";
            close(fd);
            return false;
        }
        preCreatedFDs[i] = fd;
    }

    // For HDD mode: sort by slab offset to minimize seeks
    std::vector<size_t> order(indices.size());
    std::iota(order.begin(), order.end(), 0);
    if (hddMode && mode == "random") {
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return reader.slabOffsetFor(axis, indices[a]) < reader.slabOffsetFor(axis, indices[b]);
        });
    }

    auto groupStart = std::chrono::high_resolution_clock::now();

    std::vector<float> output(sliceSize, 0.0f);
    double totalReadMs = 0, totalWriteMs = 0;

    for (size_t ordIdx = 0; ordIdx < order.size(); ++ordIdx) {
        size_t origPos = order[ordIdx];
        uint64_t sliceIdx = indices[origPos];

        // HDD: prefetch next slab while decoding current
        if (hddMode && ordIdx + 1 < order.size()) {
            reader.prefetchSlab(axis, indices[order[ordIdx + 1]]);
        }

        auto rStart = std::chrono::high_resolution_clock::now();
        if (!reader.readSlice(axis, sliceIdx, output.data())) {
            std::cerr << "\nError: readSlice failed for " << axisName << "[" << sliceIdx << "]\n";
            for (auto fd : preCreatedFDs) if (fd >= 0) close(fd);
            return false;
        }
        auto rEnd = std::chrono::high_resolution_clock::now();
        totalReadMs += std::chrono::duration<double, std::milli>(rEnd - rStart).count();

        auto wStart = std::chrono::high_resolution_clock::now();
        ssize_t written = pwrite(preCreatedFDs[origPos], output.data(), outBytes, 0);
        auto wEnd = std::chrono::high_resolution_clock::now();
        if (written != (ssize_t)outBytes) {
            std::cerr << "\nError: Write failed for " << axisName << "[" << origPos << "]\n";
            for (auto fd : preCreatedFDs) if (fd >= 0) close(fd);
            return false;
        }
        double t = std::chrono::duration<double, std::milli>(wEnd - wStart).count();
        totalWriteMs += t;
        result.perSliceTimes.push_back(t);
    }

    result.readTimeMs = totalReadMs;
    result.writeTimeMs = totalWriteMs;

    auto groupEnd = std::chrono::high_resolution_clock::now();
    result.groupTimeMs = std::chrono::duration<double, std::milli>(groupEnd - groupStart).count();

    for (auto fd : preCreatedFDs) if (fd >= 0) close(fd);
    return true;
}

void printUsage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " --input PATH --output-dir DIR [options]\n"
        << "  --random-count N      Random slices per axis (default: 100)\n"
        << "  --continuous-count N  Continuous slices per axis (default: 10)\n"
        << "  --seed N              Random seed (default: 20260511)\n"
        << "  --threads N           Thread count (default: 1)\n"
        << "  --memory-limit-mb N   Memory limit (default: 2048)\n"
        << "  --hdd                 Enable HDD mode\n";
}

int main(int argc, char* argv[]) {
    std::string inputPath, outputDir;
    int randomCount = 100, continuousCount = 10;
    uint32_t seed = 20260511;
    int numThreads = 1;
    size_t memoryLimitMB = 2048;
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
        else if (std::strcmp(argv[i], "--seed") == 0) seed = std::stoul(next());
        else if (std::strcmp(argv[i], "--threads") == 0 || std::strcmp(argv[i], "-t") == 0) numThreads = std::stoi(next());
        else if (std::strcmp(argv[i], "--memory-limit-mb") == 0 || std::strcmp(argv[i], "-m") == 0) memoryLimitMB = std::stoul(next());
        else if (std::strcmp(argv[i], "--hdd") == 0) hddMode = true;
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) { printUsage(argv[0]); return 0; }
        else { std::cerr << "Unknown option: " << argv[i] << "\n"; printUsage(argv[0]); return 1; }
    }

    if (inputPath.empty() || outputDir.empty()) {
        std::cerr << "Error: --input and --output-dir are required\n";
        return 1;
    }

    std::error_code ec;
    std::filesystem::create_directories(outputDir, ec);

    erwt3d::TriReader reader(inputPath);
    if (reader.getHeader().magic[0] == 0) {
        std::cerr << "Error: failed to open tri file\n";
        return 1;
    }

    if (hddMode) reader.setHDDMode();
    reader.setNumThreads(numThreads);
    reader.setProfileIO(true);

    const auto& header = reader.getHeader();
    uint64_t nx = header.nx, ny = header.ny, nz = header.nz;
    uint64_t rawBytes = nx * ny * nz * sizeof(float);

    struct stat fileStat;
    uint64_t fileBytes = 0;
    if (stat(inputPath.c_str(), &fileStat) == 0) fileBytes = fileStat.st_size;
    double storageRatio = (double)fileBytes / (double)rawBytes;
    int storageScore = 20;
    if (storageRatio > 1.5) {
        double over = storageRatio - 1.5;
        int penalty = (int)std::ceil(over / 0.1);
        storageScore = std::max(0, 20 - penalty);
    }

    std::cout << "============================================================\n"
              << "  ERWT3D Tri-Axis Competition Benchmark\n"
              << "============================================================\n"
              << "  File:      " << inputPath << "\n"
              << "  Dims:      " << nx << " x " << ny << " x " << nz << "\n"
              << "  Random:    " << randomCount << " slices/axis\n"
              << "  Continuous:" << continuousCount << " slices/axis\n"
              << "  Threads:   " << numThreads << "\n"
              << "  MemLimit:  " << memoryLimitMB << " MB\n"
              << "  HDD mode:  " << (hddMode ? "yes" : "no") << "\n"
              << "  Storage:   " << fileBytes << " bytes (" << std::fixed << std::setprecision(3)
              << storageRatio << "x) -> " << storageScore << "/20 pts\n"
              << "============================================================\n\n";

    // Generate indices (same as bench_contest)
    std::mt19937 rng(seed);
    std::uniform_int_distribution<uint64_t> distX(0, nx - 1);
    std::uniform_int_distribution<uint64_t> distY(0, ny - 1);
    std::uniform_int_distribution<uint64_t> distZ(0, nz - 1);

    std::vector<uint64_t> randomX(randomCount), randomY(randomCount), randomZ(randomCount);
    for (int i = 0; i < randomCount; ++i) {
        randomX[i] = distX(rng); randomY[i] = distY(rng); randomZ[i] = distZ(rng);
    }

    auto safeStart = [](uint64_t dim, int cnt) -> uint64_t {
        if ((uint64_t)cnt >= dim) return 0;
        return dim / 2 - cnt / 2;
    };
    int countX = std::min(continuousCount, (int)nx);
    int countY = std::min(continuousCount, (int)ny);
    int countZ = std::min(continuousCount, (int)nz);
    std::vector<uint64_t> continuousX(countX), continuousY(countY), continuousZ(countZ);
    uint64_t sx = safeStart(nx, countX), sy = safeStart(ny, countY), sz = safeStart(nz, countZ);
    for (int i = 0; i < countX; ++i) continuousX[i] = sx + i;
    for (int i = 0; i < countY; ++i) continuousY[i] = sy + i;
    for (int i = 0; i < countZ; ++i) continuousZ[i] = sz + i;

    struct GroupSpec {
        erwt3d::TriSliceAxis axis;
        std::string axisName, mode;
        const std::vector<uint64_t>* indices;
    };
    std::vector<GroupSpec> groups = {
        {erwt3d::TriSliceAxis::X, "x", "random",     &randomX},
        {erwt3d::TriSliceAxis::Y, "y", "random",     &randomY},
        {erwt3d::TriSliceAxis::Z, "z", "random",     &randomZ},
        {erwt3d::TriSliceAxis::X, "x", "continuous", &continuousX},
        {erwt3d::TriSliceAxis::Y, "y", "continuous", &continuousY},
        {erwt3d::TriSliceAxis::Z, "z", "continuous", &continuousZ},
    };

    std::vector<GroupResult> results(6);
    double groupTimes[6];

    std::cout << "Running 6 benchmark groups...\n\n";

    for (int g = 0; g < 6; ++g) {
        const auto& spec = groups[g];
        GroupResult gr;
        std::cout << "  [" << (g+1) << "/6] " << spec.axisName << " " << spec.mode
                  << " (" << spec.indices->size() << " slices)..." << std::flush;

        if (!runGroup(reader, spec.axis, spec.axisName, *spec.indices, spec.mode,
                      nx, ny, nz, numThreads, memoryLimitMB, outputDir, gr, hddMode)) {
            return 1;
        }

        std::cout << " " << std::fixed << std::setprecision(4) << gr.groupTimeMs / 1000.0 << "s"
                  << " (r=" << std::setprecision(3) << gr.readTimeMs / 1000.0
                  << "s w=" << gr.writeTimeMs / 1000.0 << "s)\n";

        results[g] = gr;
        groupTimes[g] = gr.groupTimeMs;
    }

    double totalAllGroups = 0;
    for (int g = 0; g < 6; ++g) totalAllGroups += groupTimes[g];
    double tComposite = totalAllGroups / 6.0;
    double avgX = (groupTimes[0] + groupTimes[3]) / 2.0;
    double avgY = (groupTimes[1] + groupTimes[4]) / 2.0;
    double avgZ = (groupTimes[2] + groupTimes[5]) / 2.0;

    // Write CSVs
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

    std::string scorePath = outputDir + "/contest_score.csv";
    {
        std::ofstream sc(scorePath);
        sc << "metric,value\n"
           << "input_file," << inputPath << "\n"
           << "dimensions," << nx << "x" << ny << "x" << nz << "\n"
           << "random_count," << randomCount << "\n"
           << "continuous_count," << continuousCount << "\n"
           << "threads," << numThreads << "\n"
           << "memory_limit_mb," << memoryLimitMB << "\n"
           << "hdd_mode," << (hddMode ? 1 : 0) << "\n"
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

    std::cout << "\n============================================================\n"
              << "  TRI-AXIS COMPETITION SCORE\n"
              << "============================================================\n\n"
              << "  6 Group Times (wall-clock, read + write):\n"
              << "    [1] X random:      " << std::setw(8) << std::fixed << std::setprecision(4)
              << groupTimes[0] / 1000.0 << "s  r=" << std::setprecision(3)
              << results[0].readTimeMs / 1000.0 << "s w=" << results[0].writeTimeMs / 1000.0 << "s\n"
              << "    [2] Y random:      " << std::setw(8) << groupTimes[1] / 1000.0
              << "s  r=" << results[1].readTimeMs / 1000.0 << "s w=" << results[1].writeTimeMs / 1000.0 << "s\n"
              << "    [3] Z random:      " << std::setw(8) << groupTimes[2] / 1000.0
              << "s  r=" << results[2].readTimeMs / 1000.0 << "s w=" << results[2].writeTimeMs / 1000.0 << "s\n"
              << "    [4] X continuous:  " << std::setw(8) << groupTimes[3] / 1000.0 << "s\n"
              << "    [5] Y continuous:  " << std::setw(8) << groupTimes[4] / 1000.0 << "s\n"
              << "    [6] Z continuous:  " << std::setw(8) << groupTimes[5] / 1000.0 << "s\n"
              << "    ----------------------------------------\n"
              << "    Total:             " << std::setw(8) << totalAllGroups / 1000.0 << "s\n"
              << "    T_composite = total/6 = " << tComposite / 1000.0 << "s\n\n"
              << "  Per-axis diagnostics:\n"
              << "    avg X = " << avgX / 1000.0 << "s\n"
              << "    avg Y = " << avgY / 1000.0 << "s\n"
              << "    avg Z = " << avgZ / 1000.0 << "s\n\n"
              << "  Storage (20pts):\n"
              << "    Ratio: " << std::setprecision(3) << storageRatio << "x\n"
              << "    Score: " << storageScore << " / 20\n\n"
              << "  Performance (60pts):\n"
              << "    T_composite = " << std::setprecision(4) << tComposite / 1000.0 << "s\n"
              << "    Score = (baseline / " << tComposite / 1000.0 << ") × 60\n\n"
              << "  Output:\n"
              << "    Score:   " << scorePath << "\n"
              << "    Summary: " << summaryPath << "\n"
              << "    Detail:  " << detailPath << "\n"
              << "============================================================\n";

    return 0;
}
