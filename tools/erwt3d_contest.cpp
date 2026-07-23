#include "erwt3d/reader.hpp"
#include "erwt3d/rzfp_reader.hpp"
#include "erwt3d/memory_budget.hpp"
#include "erwt3d/window_cache.hpp"
#include "erwt3d/format.hpp"
#include "erwt3d/rzfp_format.hpp"
#include "erwt3d/contest_positions.hpp"
#include "erwt3d/contest_groups.hpp"
#include "erwt3d/raw_x_aux.hpp"
#include "erwt3d/file_format_detect.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
constexpr uint64_t MiB = 1024ULL * 1024ULL;

static bool mkdirOne(const std::string& path) {
    if (path.empty() || path == ".") return true;
    if (mkdir(path.c_str(), 0755) == 0) return true;
    return errno == EEXIST;
}

static bool mkdirP(const std::string& path) {
    if (path.empty()) return false;
    std::string current;
    if (path.front() == '/') current = "/";
    size_t start = 0;
    while (start < path.size()) {
        const size_t slash = path.find('/', start);
        const std::string part = path.substr(
            start, slash == std::string::npos ? std::string::npos : slash - start
        );
        if (!part.empty()) {
            if (!current.empty() && current.back() != '/') current.push_back('/');
            current += part;
            if (!mkdirOne(current)) return false;
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

static void printPositions(const erwt3d::ContestPositions& pos) {
    std::cout << "X random count: " << pos.x_random.size() << "\n";
    std::cout << "Y random count: " << pos.y_random.size() << "\n";
    std::cout << "Z random count: " << pos.z_random.size() << "\n";
    if (!pos.x_continuous.empty())
        std::cout << "X continuous: " << pos.x_continuous.front() << "-" << pos.x_continuous.back() << "\n";
    if (!pos.y_continuous.empty())
        std::cout << "Y continuous: " << pos.y_continuous.front() << "-" << pos.y_continuous.back() << "\n";
    if (!pos.z_continuous.empty())
        std::cout << "Z continuous: " << pos.z_continuous.front() << "-" << pos.z_continuous.back() << "\n";
    std::cout << "Positions hash: 0x" << std::hex << erwt3d::computePositionsHash(pos) << std::dec << "\n";
}

static void writeScoreCsv(
    const std::string& path,
    const std::string& inputFile,
    const std::string& format,
    uint64_t nx, uint64_t ny, uint64_t nz,
    double storageRatio,
    int threads,
    const std::string& memoryMode,
    uint64_t memoryLimitMib,
    const erwt3d::ContestPositions& positions,
    const erwt3d::ContestUnifiedProfile& profile
) {
    std::ofstream out(path);
    out << "metric,value\n"
        << "input_file," << inputFile << '\n'
        << "format," << format << '\n'
        << "dimensions," << nx << 'x' << ny << 'x' << nz << '\n'
        << "storage_ratio," << std::fixed << std::setprecision(3) << storageRatio << '\n'
        << "threads," << threads << '\n'
        << "memory_mode," << memoryMode << '\n'
        << "memory_limit_mib," << memoryLimitMib << '\n'
        << "positions_hash,0x" << std::hex << erwt3d::computePositionsHash(positions) << std::dec << '\n'
        << "x_random_time_ms," << std::fixed << std::setprecision(3) << profile.x_random.time_ms << '\n'
        << "x_random_read_ms," << profile.x_random.read_ms << '\n'
        << "x_random_write_ms," << profile.x_random.write_ms << '\n'
        << "y_random_time_ms," << profile.y_random.time_ms << '\n'
        << "y_random_read_ms," << profile.y_random.read_ms << '\n'
        << "y_random_write_ms," << profile.y_random.write_ms << '\n'
        << "z_random_time_ms," << profile.z_random.time_ms << '\n'
        << "z_random_read_ms," << profile.z_random.read_ms << '\n'
        << "z_random_write_ms," << profile.z_random.write_ms << '\n'
        << "x_continuous_time_ms," << profile.x_continuous.time_ms << '\n'
        << "x_continuous_read_ms," << profile.x_continuous.read_ms << '\n'
        << "x_continuous_write_ms," << profile.x_continuous.write_ms << '\n'
        << "y_continuous_time_ms," << profile.y_continuous.time_ms << '\n'
        << "y_continuous_read_ms," << profile.y_continuous.read_ms << '\n'
        << "y_continuous_write_ms," << profile.y_continuous.write_ms << '\n'
        << "z_continuous_time_ms," << profile.z_continuous.time_ms << '\n'
        << "z_continuous_read_ms," << profile.z_continuous.read_ms << '\n'
        << "z_continuous_write_ms," << profile.z_continuous.write_ms << '\n'
        << "x_axis_time_ms," << profile.t_x_ms << '\n'
        << "y_axis_time_ms," << profile.t_y_ms << '\n'
        << "z_axis_time_ms," << profile.t_z_ms << '\n'
        << "T_composite_ms," << profile.t_composite_ms << '\n'
        << "process_e2e_ms," << profile.process_e2e_ms << '\n'
        << "merged_read_ms," << profile.merged_read_ms << '\n'
        << "total_write_ms," << profile.total_write_ms << '\n'
        << "total_create_files_ms," << profile.total_create_files_ms << '\n'
        << "timing_mode," << (profile.merged_read_ms > 0.0 ? "merged" : "independent") << '\n'
        << "group_read_times_estimated," << (profile.merged_read_ms > 0.0 ? "true" : "false") << '\n'
        << "output_file_count," << profile.output_file_count << '\n'
        << "output_total_bytes," << profile.output_total_bytes << '\n';
}

}

int main(int argc, char* argv[]) {
    std::string inputPath;
    std::string outputDir;
    std::string positionsFile;
    int threads = 8;
    uint64_t seed = 20260511;
    std::string memoryLimit = "auto";
    uint64_t readWindowMb = 0;

    for (int i = 1; i < argc; ++i) {
        const auto next = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << argv[i] << " requires a value\n";
                return nullptr;
            }
            return argv[++i];
        };
        if (std::strcmp(argv[i], "--input") == 0 || std::strcmp(argv[i], "-i") == 0) {
            const char* v = next(); if (!v) return 1; inputPath = v;
        } else if (std::strcmp(argv[i], "--output-dir") == 0 || std::strcmp(argv[i], "-o") == 0) {
            const char* v = next(); if (!v) return 1; outputDir = v;
        } else if (std::strcmp(argv[i], "--positions-file") == 0) {
            const char* v = next(); if (!v) return 1; positionsFile = v;
        } else if (std::strcmp(argv[i], "--threads") == 0) {
            const char* v = next(); if (!v) return 1; threads = std::stoi(v);
        } else if (std::strcmp(argv[i], "--seed") == 0) {
            const char* v = next(); if (!v) return 1; seed = std::stoull(v);
        } else if (std::strcmp(argv[i], "--memory-limit-mb") == 0) {
            const char* v = next(); if (!v) return 1; memoryLimit = v;
        } else if (std::strcmp(argv[i], "--read-window-mb") == 0) {
            const char* v = next(); if (!v) return 1; readWindowMb = std::stoull(v);
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::cerr
                << "Usage: erwt3d_contest --input DATA.erwt3d --output-dir DIR [options]\n\n"
                << "Official competition entrypoint\n\n"
                << "  --input PATH           ERWT3D file (LZ4 or RZFP, auto-detected)\n"
                << "  --output-dir DIR       Output directory (required)\n"
                << "  --positions-file PATH  CSV/TXT coordinate file (official mode)\n"
                << "  --seed N               Random seed for test mode (default: 20260511)\n"
                << "  --threads N            Thread count (default: 8)\n"
                << "  --memory-limit-mb auto|N    Memory limit MB (default: auto=70% MemAvailable)\n"
                << "  --read-window-mb N          Max read window MB (0=auto, max 128, default: 0)\n\n"
                << "Official mode (--positions-file):\n"
                << "  X/Y/Z random: 100 each, no duplicates\n"
                << "  X/Y/Z continuous: 10 each, strictly consecutive\n\n"
                << "Test mode (no --positions-file):\n"
                << "  Generates random positions with --seed\n";
            return 0;
        } else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            return 1;
        }
    }

    if (inputPath.empty() || outputDir.empty()) {
        std::cerr << "Error: --input and --output-dir are required\n";
        return 1;
    }

    if (!mkdirP(outputDir)) {
        std::cerr << "Error: cannot create output directory " << outputDir << "\n";
        return 1;
    }

    const erwt3d::OptimizedFileFormat fmt = erwt3d::detectOptimizedFileFormat(inputPath);
    if (fmt == erwt3d::OptimizedFileFormat::Unknown) {
        std::cerr << "Error: cannot detect file format (not ERWT3D or RZFP)\n";
        return 1;
    }

    const char* fmtName = (fmt == erwt3d::OptimizedFileFormat::LZ4_ERWT3D) ? "LZ4" : "RZFP";

    uint64_t nx = 0, ny = 0, nz = 0;
    double storageRatio = 0.0;

    if (fmt == erwt3d::OptimizedFileFormat::LZ4_ERWT3D) {
        erwt3d::ERWT3DReader reader(inputPath);
        const auto& header = reader.getHeader();
        nx = header.nx; ny = header.ny; nz = header.nz;
        uint64_t rawBytes = erwt3d::getRawSize(header);
        struct stat st{};
        uint64_t fileBytes = 0;
        if (stat(inputPath.c_str(), &st) == 0) fileBytes = st.st_size;
        const uint64_t totalBytes = erwt3d::getTotalOptimizedStorageBytes(
            inputPath, fileBytes, header);
        storageRatio = rawBytes > 0 ? static_cast<double>(totalBytes) / rawBytes : 0.0;
    } else {
        erwt3d::RzfpReader reader(inputPath);
        if (!reader.ok()) {
            std::cerr << "Error: cannot open RZFP file " << inputPath << "\n";
            return 1;
        }
        const auto& header = reader.header();
        nx = header.nx; ny = header.ny; nz = header.nz;
        struct stat st{};
        uint64_t fileBytes = 0;
        if (stat(inputPath.c_str(), &st) == 0) fileBytes = st.st_size;
        const uint64_t rawBytes = erwt3d::rzfpRawSize(header);
        storageRatio = rawBytes > 0 ? static_cast<double>(fileBytes) / rawBytes : 0.0;
    }

    const uint32_t randomCount = 100;
    const uint32_t continuousCount = 10;
    erwt3d::ContestPositions positions;
    std::string posError;

    if (!positionsFile.empty()) {
        if (!erwt3d::parsePositionsFile(positionsFile, nx, ny, nz,
                                         randomCount, continuousCount,
                                         positions, posError)) {
            std::cerr << "Error: " << posError << "\n";
            return 1;
        }
    } else {
        if (!erwt3d::generateRandomPositions(nx, ny, nz,
                                              randomCount, continuousCount,
                                              seed, positions, posError)) {
            std::cerr << "Error: " << posError << "\n";
            return 1;
        }
        const std::string posCsvPath = outputDir + "/contest_positions.csv";
        std::ofstream posOut(posCsvPath);
        posOut << "axis,type,index\n";
        for (auto x : positions.x_random) posOut << "x,random," << x << "\n";
        for (auto y : positions.y_random) posOut << "y,random," << y << "\n";
        for (auto z : positions.z_random) posOut << "z,random," << z << "\n";
        for (auto x : positions.x_continuous) posOut << "x,continuous," << x << "\n";
        for (auto y : positions.y_continuous) posOut << "y,continuous," << y << "\n";
        for (auto z : positions.z_continuous) posOut << "z,continuous," << z << "\n";
    }

    std::cout << "============================================================\n"
              << "  ERWT3D Contest Entry (" << fmtName << " path)\n"
              << "============================================================\n"
              << "  File:          " << inputPath << "\n"
              << "  Dims:          " << nx << " x " << ny << " x " << nz << "\n"
              << "  Format:        " << fmtName << "\n"
              << "  Threads:       " << threads << "\n"
              << "  Storage ratio: " << std::fixed << std::setprecision(3) << storageRatio << "x\n"
              << "============================================================\n\n";

    printPositions(positions);
    std::cout << "\n";

    erwt3d::ContestReadBatchFunction readFn;

    if (fmt == erwt3d::OptimizedFileFormat::LZ4_ERWT3D) {
        auto reader = std::make_shared<erwt3d::ERWT3DReader>(inputPath);

        size_t memoryLimitMB = 4096;
        if (memoryLimit == "auto" || memoryLimit == "0") {
            uint64_t memAvail = erwt3d::readLinuxMemAvailableBytes();
            memoryLimitMB = memAvail > 4ULL * 1024 * 1024 * 1024
                ? static_cast<size_t>(memAvail * 0.70 / (1024 * 1024))
                : static_cast<size_t>(memAvail / 2 / (1024 * 1024));
        } else {
            memoryLimitMB = std::stoull(memoryLimit);
        }

        reader->setIOBackend(erwt3d::IOBackend::Superblock);
        reader->setSBReadMode(erwt3d::SBReadMode::HDDReadWindow);
        reader->setSBTaskOrder(erwt3d::SBTaskOrder::FileOffset);
        reader->setHDDReadWindowConfig({
            readWindowMb > 0 ? std::min<uint64_t>(readWindowMb * MiB, 128ULL * MiB) : 128ULL * MiB,
            8ULL * MiB
        });

        readFn = [reader, threads, memoryLimitMB, readWindowMb](
            erwt3d::SliceAxis axis,
            const std::vector<uint64_t>& indices,
            std::vector<std::vector<float>>& outputs
        ) -> bool {
            erwt3d::HDDReadWindowConfig wcfg{
                readWindowMb > 0 ? std::min<uint64_t>(readWindowMb * MiB, 128ULL * MiB) : 128ULL * MiB,
                8ULL * MiB
            };

            std::vector<erwt3d::ERWT3DReader::SliceBatchRequest> reqs;
            for (size_t i = 0; i < indices.size(); ++i) {
                reqs.push_back({axis, indices[i], outputs[i].data()});
            }
            return reader->readSlicesBatch(reqs, threads, memoryLimitMB, wcfg);
        };

    } else {
        auto reader = std::make_shared<erwt3d::RzfpReader>(inputPath);
        if (!reader->ok()) {
            std::cerr << "Error: cannot open RZFP file\n";
            return 1;
        }

        const auto& header = reader->header();
        uint64_t largestOutputBytes = std::max({header.ny * header.nz * sizeof(float),
                                                 header.nx * header.nz * sizeof(float),
                                                 header.nx * header.ny * sizeof(float)});

        erwt3d::MemoryBudget budget = erwt3d::makeMemoryBudget(
            memoryLimit, reader->payloadBytes(), largestOutputBytes, 100);

        auto windowCache = std::make_shared<erwt3d::BoundedWindowCache>(budget.window_cache_bytes);

        erwt3d::RzfpReaderConfig config;
        config.strategy = erwt3d::RzfpReadStrategy::Auto;
        config.decode_threads = threads;
        config.window_cache = windowCache;
        config.window_cache_file_identity = reader->fileIdentity();
        config.use_window_cache = true;
        config.adaptive.auto_calibrate_device = false;
        config.adaptive.cache_policy = erwt3d::CachePolicy::StableAuto;
        config.hdd.sequential_mb_s = 250.0;
        config.hdd.seek_ms = 10.0;
        config.hdd.read_window_bytes = readWindowMb > 0
            ? std::min<uint64_t>(readWindowMb * MiB, 128ULL * MiB)
            : std::min<uint64_t>(128ULL * MiB, budget.window_cache_bytes / 2 > 0
                ? budget.window_cache_bytes / 2 : 64ULL * MiB);
        config.hdd.max_gap_bytes = 8ULL * MiB;

        readFn = [reader, config](
            erwt3d::SliceAxis axis,
            const std::vector<uint64_t>& indices,
            std::vector<std::vector<float>>& outputs
        ) -> bool {
            if (indices.empty()) return true;

            std::vector<erwt3d::RzfpReader::SliceBatchRequest> reqs;
            for (size_t i = 0; i < indices.size(); ++i) {
                reqs.push_back({axis, indices[i], outputs[i].data()});
            }
            return reader->readSlicesBatch(reqs, config);
        };
    }

    erwt3d::ContestUnifiedProfile profile;

    if (fmt == erwt3d::OptimizedFileFormat::LZ4_ERWT3D) {
        if (!erwt3d::executeContestGroups(positions, outputDir, nx, ny, nz, readFn, &profile)) {
            std::cerr << "Error: contest execution failed\n";
            return 1;
        }
    } else {
        auto rzfpReader = std::make_shared<erwt3d::RzfpReader>(inputPath);
        if (!rzfpReader->ok()) {
            std::cerr << "Error: cannot open RZFP file\n";
            return 1;
        }

        const auto& rzfpHeader = rzfpReader->header();
        uint64_t largestOutputBytes = std::max({rzfpHeader.ny * rzfpHeader.nz * sizeof(float),
                                                 rzfpHeader.nx * rzfpHeader.nz * sizeof(float),
                                                 rzfpHeader.nx * rzfpHeader.ny * sizeof(float)});

        erwt3d::MemoryBudget budget = erwt3d::makeMemoryBudget(
            memoryLimit, rzfpReader->payloadBytes(), largestOutputBytes, 100);

        auto windowCache = std::make_shared<erwt3d::BoundedWindowCache>(budget.window_cache_bytes);

        erwt3d::RzfpReaderConfig rzfpConfig;
        rzfpConfig.strategy = erwt3d::RzfpReadStrategy::Auto;
        rzfpConfig.decode_threads = threads;
        rzfpConfig.window_cache = windowCache;
        rzfpConfig.window_cache_file_identity = rzfpReader->fileIdentity();
        rzfpConfig.use_window_cache = true;
        rzfpConfig.adaptive.auto_calibrate_device = false;
        rzfpConfig.adaptive.cache_policy = erwt3d::CachePolicy::StableAuto;
        rzfpConfig.hdd.sequential_mb_s = 250.0;
        rzfpConfig.hdd.seek_ms = 10.0;
        rzfpConfig.hdd.read_window_bytes = readWindowMb > 0
            ? std::min<uint64_t>(readWindowMb * MiB, 128ULL * MiB)
            : std::min<uint64_t>(128ULL * MiB, budget.window_cache_bytes / 2 > 0
                ? budget.window_cache_bytes / 2 : 64ULL * MiB);
        rzfpConfig.hdd.max_gap_bytes = 8ULL * MiB;

        erwt3d::MultiGroupReadFunction mergedFn =
            [rzfpReader, rzfpConfig](
                const std::vector<erwt3d::GroupReadEntry>& groups,
                std::vector<std::vector<std::vector<float>>>& allOutputs
            ) -> bool {
            if (groups.empty()) return true;

            std::vector<erwt3d::RzfpReader::ContestRoundGroup> cgroups(groups.size());
            for (size_t g = 0; g < groups.size(); ++g) {
                cgroups[g].axis = groups[g].axis;
                cgroups[g].name = groups[g].name;
                cgroups[g].indices = groups[g].indices;
                cgroups[g].outputs.clear();
                for (auto& o : allOutputs[g]) cgroups[g].outputs.push_back(o.data());
            }

            std::vector<erwt3d::RzfpReader::RzfpRoundReadResult> results;
            return rzfpReader->readContestRound(cgroups, rzfpConfig, &results);
        };

        if (!erwt3d::executeContestGroupsMerged(positions, outputDir, nx, ny, nz, mergedFn, &profile)) {
            std::cerr << "Error: contest execution failed\n";
            return 1;
        }
    }

    std::cout << std::fixed << std::setprecision(3);

    auto printGroup = [&](const char* label, const erwt3d::ContestGroupTiming& t) {
        const char* suffix = (profile.merged_read_ms > 0.0) ? " (est share)" : "";
        std::cout << label << suffix << ":     " << t.time_ms / 1000.0 << " s"
                  << "  (read=" << t.read_ms / 1000.0
                  << "s write=" << t.write_ms / 1000.0
                  << "s";
        if (profile.merged_read_ms == 0.0) {
            std::cout << " create=" << t.create_files_ms / 1000.0 << "s";
        }
        std::cout << ")\n";
    };

    printGroup("X random     ", profile.x_random);
    printGroup("Y random     ", profile.y_random);
    printGroup("Z random     ", profile.z_random);
    printGroup("X continuous ", profile.x_continuous);
    printGroup("Y continuous ", profile.y_continuous);
    printGroup("Z continuous ", profile.z_continuous);
    std::cout << "T_X:            " << profile.t_x_ms / 1000.0 << " s\n";
    std::cout << "T_Y:            " << profile.t_y_ms / 1000.0 << " s\n";
    std::cout << "T_Z:            " << profile.t_z_ms / 1000.0 << " s\n";
    std::cout << "T_composite:    " << profile.t_composite_ms / 1000.0 << " s\n";
    std::cout << "Process e2e:    " << profile.process_e2e_ms / 1000.0 << " s\n";
    if (profile.merged_read_ms > 0.0) {
        std::cout << "  merged_read:  " << profile.merged_read_ms / 1000.0 << " s\n";
        std::cout << "  total_write:  " << profile.total_write_ms / 1000.0 << " s\n";
        std::cout << "  total_create: " << profile.total_create_files_ms / 1000.0 << " s\n";
    }
    std::cout << "Output files:   " << profile.output_file_count << "\n";

    const std::string scorePath = outputDir + "/contest_score.csv";
    writeScoreCsv(scorePath, inputPath, fmtName, nx, ny, nz, storageRatio,
                  threads, memoryLimit, 0, positions, profile);

    std::cout << "\nScore written to " << scorePath << "\n";
    return 0;
}
