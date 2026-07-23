#include "erwt3d/reader.hpp"
#include "erwt3d/rzfp_reader.hpp"
#include "erwt3d/memory_budget.hpp"
#include "erwt3d/window_cache.hpp"
#include "erwt3d/format.hpp"
#include "erwt3d/rzfp_format.hpp"
#include "erwt3d/contest_round_executor.hpp"
#include "erwt3d/raw_x_aux.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
constexpr uint64_t MiB = 1024ULL * 1024ULL;

enum class FileFormat { LZ4, RZFP, Unknown };

static FileFormat detectFormat(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return FileFormat::Unknown;
    char magic[8] = {};
    ssize_t rd = pread(fd, magic, 8, 0);
    close(fd);
    if (rd < 8) return FileFormat::Unknown;
    if (std::memcmp(magic, erwt3d::ERWT3D_MAGIC, 8) == 0) return FileFormat::LZ4;
    if (std::memcmp(magic, erwt3d::RZFP_MAGIC, 8) == 0) return FileFormat::RZFP;
    return FileFormat::Unknown;
}

static uint64_t sliceElem(uint64_t nx, uint64_t ny, uint64_t nz, erwt3d::SliceAxis axis) {
    switch (axis) {
        case erwt3d::SliceAxis::X: return ny * nz;
        case erwt3d::SliceAxis::Y: return nx * nz;
        case erwt3d::SliceAxis::Z: return nx * ny;
    }
    return 0;
}

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

} // namespace

int main(int argc, char* argv[]) {
    std::string inputPath;
    std::string outputDir;
    int randomCount = 100;
    int continuousCount = 10;
    int threads = 8;
    uint32_t seed = 20260511;
    std::string memoryLimit = "auto";
    uint64_t readWindowMb = 0;

    for (int i = 1; i < argc; ++i) {
        const auto next = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << argv[i] << " requires a value\n";
                std::exit(1);
            }
            return argv[++i];
        };
        if (std::strcmp(argv[i], "--input") == 0 || std::strcmp(argv[i], "-i") == 0) {
            inputPath = next();
        } else if (std::strcmp(argv[i], "--output-dir") == 0 || std::strcmp(argv[i], "-o") == 0) {
            outputDir = next();
        } else if (std::strcmp(argv[i], "--random-count") == 0) {
            randomCount = std::stoi(next());
        } else if (std::strcmp(argv[i], "--continuous-count") == 0) {
            continuousCount = std::stoi(next());
        } else if (std::strcmp(argv[i], "--threads") == 0) {
            threads = std::stoi(next());
        } else if (std::strcmp(argv[i], "--seed") == 0) {
            seed = static_cast<uint32_t>(std::stoul(next()));
        } else if (std::strcmp(argv[i], "--memory-limit-mb") == 0) {
            memoryLimit = next();
        } else if (std::strcmp(argv[i], "--read-window-mb") == 0) {
            readWindowMb = std::stoull(next());
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::cerr
                << "Usage: erwt3d_contest --input DATA.erwt3d --output-dir DIR [options]\n\n"
                << "Official competition entrypoint (赛题2 正式入口)\n\n"
                << "  --input PATH           ERWT3D file (LZ4 or RZFP, auto-detected)\n"
                << "  --output-dir DIR       Output directory (required)\n"
                << "  --random-count N       Random slices per axis (default: 100)\n"
                << "  --continuous-count N   Continuous slices per axis (default: 10)\n"
                << "  --threads N            Thread count (default: 8)\n"
                << "  --memory-limit-mb auto|N    Memory limit MB (default: auto=70% MemAvailable)\n"
                << "  --read-window-mb N          Max read window MB (0=auto, max 128, default: 0)\n"
                << "  --seed N               Random seed (default: 20260511)\n";
            return 0;
        } else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            return 1;
        }
    }

    if (inputPath.empty() || outputDir.empty() ||
        randomCount <= 0 || continuousCount <= 0 || threads <= 0) {
        std::cerr << "Error: invalid or missing required arguments\n";
        return 1;
    }

    const auto e2eStart = Clock::now();

    if (!mkdirP(outputDir)) {
        std::cerr << "Error: cannot create output directory " << outputDir << "\n";
        return 1;
    }

    const FileFormat fmt = detectFormat(inputPath);
    if (fmt == FileFormat::Unknown) {
        std::cerr << "Error: cannot detect file format (not ERWT3D or RZFP)\n";
        return 1;
    }

    const char* fmtName = (fmt == FileFormat::LZ4) ? "LZ4" : "RZFP";

    if (fmt == FileFormat::LZ4) {
        // ========== LZ4 path ==========
        erwt3d::ERWT3DReader reader(inputPath);
        const auto& header = reader.getHeader();

        uint64_t nx = header.nx, ny = header.ny, nz = header.nz;
        uint64_t rawBytes = erwt3d::getRawSize(header);

        struct stat st{};
        uint64_t fileBytes = 0;
        if (stat(inputPath.c_str(), &st) == 0) fileBytes = st.st_size;
        const std::string sidecarPath = inputPath + ".xp";
        if (stat(sidecarPath.c_str(), &st) == 0) fileBytes += st.st_size;
        double storageRatio = rawBytes > 0 ? static_cast<double>(fileBytes) / rawBytes : 0.0;

        std::mt19937 rng(seed);
        std::uniform_int_distribution<uint64_t> xDist(0, nx - 1);
        std::uniform_int_distribution<uint64_t> yDist(0, ny - 1);
        std::uniform_int_distribution<uint64_t> zDist(0, nz - 1);

        std::vector<uint64_t> rndX(randomCount), rndY(randomCount), rndZ(randomCount);
        for (int i = 0; i < randomCount; ++i) {
            rndX[i] = xDist(rng); rndY[i] = yDist(rng); rndZ[i] = zDist(rng);
        }

        auto makeCont = [](uint64_t dim, int cnt) {
            const int n = std::min<int>(cnt, static_cast<int>(dim));
            std::vector<uint64_t> v(static_cast<size_t>(n));
            const uint64_t start = static_cast<uint64_t>(n) >= dim ? 0 : dim / 2 - static_cast<uint64_t>(n) / 2;
            for (int i = 0; i < n; ++i) v[i] = start + static_cast<uint64_t>(i);
            return v;
        };
        const auto contX = makeCont(nx, continuousCount);
        const auto contY = makeCont(ny, continuousCount);
        const auto contZ = makeCont(nz, continuousCount);

        struct GroupDef {
            erwt3d::SliceAxis axis;
            std::string name;
            const std::vector<uint64_t>* indices;
        };
        const std::vector<GroupDef> groups = {
            {erwt3d::SliceAxis::X, "x_random", &rndX},
            {erwt3d::SliceAxis::Y, "y_random", &rndY},
            {erwt3d::SliceAxis::Z, "z_random", &rndZ},
            {erwt3d::SliceAxis::X, "x_continuous", &contX},
            {erwt3d::SliceAxis::Y, "y_continuous", &contY},
            {erwt3d::SliceAxis::Z, "z_continuous", &contZ},
        };

        size_t memoryLimitMB = 4096;
        if (memoryLimit == "auto" || memoryLimit == "0") {
            uint64_t memAvail = erwt3d::readLinuxMemAvailableBytes();
            memoryLimitMB = memAvail > 4ULL * 1024 * 1024 * 1024
                ? static_cast<size_t>(memAvail * 0.70 / (1024 * 1024))
                : static_cast<size_t>(memAvail / 2 / (1024 * 1024));
        } else {
            memoryLimitMB = std::stoull(memoryLimit);
        }

        reader.setIOBackend(erwt3d::IOBackend::Superblock);
        reader.setSBReadMode(erwt3d::SBReadMode::HDDReadWindow);
        reader.setSBTaskOrder(erwt3d::SBTaskOrder::FileOffset);
        reader.setHDDReadWindowConfig({
            readWindowMb > 0 ? std::min<uint64_t>(readWindowMb * MiB, 128ULL * MiB) : 128ULL * MiB,
            8ULL * MiB
        });

        std::cout
            << "============================================================\n"
            << "  ERWT3D Contest Entry (LZ4 path)\n"
            << "============================================================\n"
            << "  File:          " << inputPath << "\n"
            << "  Dims:          " << nx << " x " << ny << " x " << nz << "\n"
            << "  Format:        LZ4\n"
            << "  Threads:       " << threads << "\n"
            << "  Memory limit:  " << memoryLimitMB << " MiB\n"
            << "  Storage ratio: " << std::setprecision(3) << storageRatio << "x\n"
            << "============================================================\n\n";

        double totalGroupMs = 0.0;
        for (size_t g = 0; g < groups.size(); ++g) {
            const auto& gd = groups[g];
            uint64_t elem = sliceElem(nx, ny, nz, gd.axis);
            uint64_t outBytes = elem * sizeof(float);

            auto gStart = Clock::now();

            std::vector<int> fds(gd.indices->size(), -1);
            for (size_t i = 0; i < gd.indices->size(); ++i) {
                std::ostringstream oss;
                oss << outputDir << "/contest_g" << g << "_" << gd.name << "_" << i << ".raw";
                int fd = open(oss.str().c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
                if (fd >= 0) {
                    posix_fallocate(fd, 0, static_cast<off_t>(outBytes));
                    fds[i] = fd;
                }
            }

            size_t batchSize = std::min<size_t>(
                gd.indices->size(),
                std::max<size_t>(1, static_cast<size_t>(memoryLimitMB) * 1024 * 1024 / (outBytes + 1) / 2)
            );
            for (size_t batchStart = 0; batchStart < gd.indices->size(); batchStart += batchSize) {
                size_t batchEnd = std::min(batchStart + batchSize, gd.indices->size());
                std::vector<erwt3d::ERWT3DReader::SliceBatchRequest> reqs;
                std::vector<std::vector<float>> buffers(batchEnd - batchStart);
                for (size_t i = batchStart; i < batchEnd; ++i) {
                    buffers[i - batchStart].resize(elem);
                    reqs.push_back({gd.axis, (*gd.indices)[i], buffers[i - batchStart].data()});
                }
                erwt3d::HDDReadWindowConfig wcfg{
                    readWindowMb > 0 ? std::min<uint64_t>(readWindowMb * MiB, 128ULL * MiB) : 128ULL * MiB,
                    8ULL * MiB
                };
                reader.readSlicesBatch(reqs, threads, memoryLimitMB, wcfg);

                for (size_t i = batchStart; i < batchEnd; ++i) {
                    if (fds[i] >= 0) {
                        erwt3d::writeFullyAt(fds[i], buffers[i - batchStart].data(), outBytes, 0);
                    }
                }
            }

            for (int fd : fds) if (fd >= 0) close(fd);

            double gMs = std::chrono::duration<double, std::milli>(Clock::now() - gStart).count();
            totalGroupMs += gMs;

            std::cerr << "  [" << (g+1) << "/6] " << gd.name
                      << " " << std::fixed << std::setprecision(3) << gMs / 1000.0 << "s" << std::endl;
        }

        double e2eMs = std::chrono::duration<double, std::milli>(Clock::now() - e2eStart).count();
        double compositeMs = e2eMs / static_cast<double>(groups.size());

        std::cout << std::fixed << std::setprecision(3);
        std::cout << "Group total:    " << totalGroupMs / 1000.0 << " s\n";
        std::cout << "Process e2e:    " << e2eMs / 1000.0 << " s\n";
        std::cout << "T_composite:    " << compositeMs / 1000.0 << " s (e2e/6)\n";

        const std::string scorePath = outputDir + "/contest_score.csv";
        {
            std::ofstream out(scorePath);
            out << "metric,value\n"
                << "input_file," << inputPath << '\n'
                << "format,LZ4\n"
                << "dimensions," << nx << 'x' << ny << 'x' << nz << '\n'
                << "storage_ratio," << storageRatio << '\n'
                << "threads," << threads << '\n'
                << "memory_limit_mib," << memoryLimitMB << '\n'
                << "total_time_ms," << totalGroupMs << '\n'
                << "T_composite_ms," << compositeMs << '\n'
                << "process_e2e_ms," << e2eMs << '\n';
        }
        std::cout << "\nScore written to " << scorePath << "\n";
        return 0;
    }

    // ========== RZFP path ==========
    erwt3d::RzfpReader reader(inputPath);
    if (!reader.ok()) {
        std::cerr << "Error: cannot open RZFP file " << inputPath << "\n";
        return 1;
    }
    const auto& header = reader.header();

    uint64_t largestOutputBytes = 0;
    largestOutputBytes = std::max(largestOutputBytes, header.ny * header.nz * sizeof(float));
    largestOutputBytes = std::max(largestOutputBytes, header.nx * header.nz * sizeof(float));
    largestOutputBytes = std::max(largestOutputBytes, header.nx * header.ny * sizeof(float));

    erwt3d::MemoryBudget budget = erwt3d::makeMemoryBudget(
        memoryLimit,
        reader.payloadBytes(),
        largestOutputBytes,
        static_cast<uint64_t>(std::max(randomCount, continuousCount))
    );
    if (!budget.valid) {
        std::cerr << "Error: memory budget: " << budget.error << "\n";
        return 1;
    }

    auto windowCache = std::make_shared<erwt3d::BoundedWindowCache>(
        budget.window_cache_bytes
    );

    struct stat st{};
    uint64_t fileBytes = 0;
    if (stat(inputPath.c_str(), &st) == 0) fileBytes = static_cast<uint64_t>(st.st_size);
    const std::string sidecarPath = inputPath + ".xp";
    if (stat(sidecarPath.c_str(), &st) == 0) fileBytes += static_cast<uint64_t>(st.st_size);
    const uint64_t rawBytes = erwt3d::rzfpRawSize(header);
    const double storageRatio = rawBytes > 0
        ? static_cast<double>(fileBytes) / static_cast<double>(rawBytes) : 0.0;

    std::mt19937 rng(seed);
    std::uniform_int_distribution<uint64_t> xDist(0, header.nx - 1);
    std::uniform_int_distribution<uint64_t> yDist(0, header.ny - 1);
    std::uniform_int_distribution<uint64_t> zDist(0, header.nz - 1);

    std::vector<uint64_t> rndX(randomCount), rndY(randomCount), rndZ(randomCount);
    for (int i = 0; i < randomCount; ++i) {
        rndX[i] = xDist(rng);
        rndY[i] = yDist(rng);
        rndZ[i] = zDist(rng);
    }

    auto makeCont = [](uint64_t dim, int cnt) {
        const int n = std::min<int>(cnt, static_cast<int>(dim));
        std::vector<uint64_t> v(static_cast<size_t>(n));
        const uint64_t start = static_cast<uint64_t>(n) >= dim ? 0 : dim / 2 - static_cast<uint64_t>(n) / 2;
        for (int i = 0; i < n; ++i) v[i] = start + static_cast<uint64_t>(i);
        return v;
    };
    const auto contX = makeCont(header.nx, continuousCount);
    const auto contY = makeCont(header.ny, continuousCount);
    const auto contZ = makeCont(header.nz, continuousCount);

    struct GroupDef {
        erwt3d::SliceAxis axis;
        std::string name;
        const std::vector<uint64_t>* indices;
    };
    const std::vector<GroupDef> groups = {
        {erwt3d::SliceAxis::X, "x_random", &rndX},
        {erwt3d::SliceAxis::Y, "y_random", &rndY},
        {erwt3d::SliceAxis::Z, "z_random", &rndZ},
        {erwt3d::SliceAxis::X, "x_continuous", &contX},
        {erwt3d::SliceAxis::Y, "y_continuous", &contY},
        {erwt3d::SliceAxis::Z, "z_continuous", &contZ},
    };

    erwt3d::RzfpReaderConfig config;
    config.strategy = erwt3d::RzfpReadStrategy::Auto;
    config.decode_threads = threads;
    config.window_cache = windowCache;
    config.window_cache_file_identity = reader.fileIdentity();
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

    std::cout
        << "============================================================\n"
        << "  ERWT3D Contest Entry (RZFP path)\n"
        << "============================================================\n"
        << "  File:          " << inputPath << "\n"
        << "  Dims:          " << header.nx << " x "
        << header.ny << " x " << header.nz << "\n"
        << "  Format:        RZFP\n"
        << "  Threads:       " << threads << "\n"
        << "  Device:        250.0 MB/s (preset)\n"
        << "  Memory mode:   " << (budget.automatic ? "AUTO" : "MANUAL") << "\n";
    if (budget.automatic) {
        std::cout
            << "  MemAvailable:  " << budget.auto_mem_available / MiB << " MiB\n"
            << "  AUTO fraction: 70%\n"
            << "  Memory hard limit: " << budget.total_bytes / MiB << " MiB\n"
            << "  Auto reserve:  " << budget.auto_reserve / MiB << " MiB\n";
    } else {
        std::cout
            << "  Memory limit:  " << budget.total_bytes / MiB << " MiB\n";
    }
    std::cout
        << "  Window cache:  " << budget.window_cache_bytes / MiB << " MiB\n"
        << "  Output batch:  " << budget.output_batch_size << " slices\n"
        << "  IO buffer:     " << budget.io_buffer_bytes / MiB << " MiB\n"
        << "  Read window:   " << config.hdd.read_window_bytes / MiB << " MiB\n"
        << "  Reserve:       " << budget.reserve_bytes / MiB << " MiB\n"
        << "  Storage ratio: " << std::setprecision(3) << storageRatio << "x\n"
        << "============================================================\n\n";

    std::vector<erwt3d::ContestExecutionGroup> execGroups;
    for (size_t g = 0; g < groups.size(); ++g) {
        erwt3d::ContestExecutionGroup eg;
        eg.axis = groups[g].axis;
        eg.name = groups[g].name;
        eg.indices = groups[g].indices;
        execGroups.push_back(eg);
    }

    erwt3d::ContestExecutionProfile execProfile;
    if (!erwt3d::executeContestRound(
            reader, header, execGroups, outputDir, "contest",
            config, budget, &execProfile)) {
        std::cerr << "Error: contest round execution failed\n";
        return 1;
    }

    const double e2eMs = std::chrono::duration<double, std::milli>(Clock::now() - e2eStart).count();
    const double readMs = execProfile.read_time_ms;
    const double writeMs = execProfile.write_time_ms;
    const double setupMs = execProfile.setup_time_ms;
    const double prepareMs = execProfile.output_prepare_ms;
    const double closeMs = execProfile.close_time_ms;
    const double totalMs = execProfile.total_time_ms;
    const double compositeMs = totalMs / static_cast<double>(groups.size());

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "Setup time:     " << setupMs / 1000.0 << " s\n";
    std::cout << "Prepare time:   " << prepareMs / 1000.0 << " s\n";
    std::cout << "Read time:      " << readMs / 1000.0 << " s\n";
    std::cout << "Write time:     " << writeMs / 1000.0 << " s\n";
    std::cout << "Close time:     " << closeMs / 1000.0 << " s\n";
    std::cout << "Total (e2e):    " << totalMs / 1000.0 << " s\n";
    std::cout << "T_composite:    " << compositeMs / 1000.0 << " s\n";
    std::cout << "Process e2e:    " << e2eMs / 1000.0 << " s\n";

    const std::string scorePath = outputDir + "/contest_score.csv";
    {
        std::ofstream out(scorePath);
        out << "metric,value\n"
            << "input_file," << inputPath << '\n'
            << "format,RZFP\n"
            << "dimensions," << header.nx << 'x' << header.ny << 'x' << header.nz << '\n'
            << "storage_ratio," << storageRatio << '\n'
            << "threads," << threads << '\n'
            << "memory_mode," << (budget.automatic ? "AUTO" : "MANUAL") << '\n'
            << "mem_available_mib," << budget.auto_mem_available / MiB << '\n'
            << "memory_limit_mib," << budget.total_bytes / MiB << '\n'
            << "window_cache_mib," << budget.window_cache_bytes / MiB << '\n'
            << "output_batch," << budget.output_batch_size << '\n'
            << "io_buffer_mib," << budget.io_buffer_bytes / MiB << '\n'
            << "read_window_mib," << config.hdd.read_window_bytes / MiB << '\n'
            << "reserve_mib," << budget.reserve_bytes / MiB << '\n'
            << "setup_time_ms," << setupMs << '\n'
            << "output_prepare_ms," << prepareMs << '\n'
            << "read_time_ms," << readMs << '\n'
            << "write_time_ms," << writeMs << '\n'
            << "close_time_ms," << closeMs << '\n'
            << "total_time_ms," << totalMs << '\n'
            << "T_composite_ms," << compositeMs << '\n'
            << "process_e2e_ms," << e2eMs << '\n'
            << "logical_leaf_requests," << execProfile.logical_leaf_requests << '\n'
            << "duplicate_leaf_requests," << execProfile.duplicate_leaf_requests << '\n'
            << "eliminated_record_bytes," << execProfile.eliminated_record_bytes << '\n'
            << "actual_read_bytes," << execProfile.actual_read_bytes << '\n';
        if (execProfile.selected_strategy != erwt3d::RzfpReadStrategy::Auto) {
            out << "selected_strategy," << static_cast<int>(execProfile.selected_strategy) << '\n';
        }
        if (!execProfile.strategy_reason.empty()) {
            out << "strategy_reason," << execProfile.strategy_reason << '\n';
        }
    }

    std::cout << "\nScore written to " << scorePath << "\n";
    return 0;
}
