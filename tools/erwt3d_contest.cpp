#include "erwt3d/rzfp_reader.hpp"
#include "erwt3d/memory_budget.hpp"
#include "erwt3d/window_cache.hpp"
#include "erwt3d/rzfp_format.hpp"
#include "erwt3d/contest_round_executor.hpp"

#include <algorithm>
#include <cerrno>
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

static uint64_t sliceElements(
    const erwt3d::RzfpFileHeader& header,
    erwt3d::SliceAxis axis
) {
    switch (axis) {
        case erwt3d::SliceAxis::X: return header.ny * header.nz;
        case erwt3d::SliceAxis::Y: return header.nx * header.nz;
        case erwt3d::SliceAxis::Z: return header.nx * header.ny;
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

static bool precreateOutputs(
    const std::string& output_dir,
    const std::string& prefix,
    size_t count,
    uint64_t bytes,
    std::vector<int>& fds
) {
    fds.clear();
    fds.resize(count, -1);
    for (size_t i = 0; i < count; ++i) {
        std::ostringstream oss;
        oss << output_dir << "/" << prefix << "_" << i << ".raw";
        const std::string path = oss.str();
        int fd = open(
            path.c_str(),
            O_RDWR | O_CREAT | O_TRUNC,
            0644
        );
        if (fd < 0) {
            std::cerr << "Error: cannot create output " << path << "\n";
            return false;
        }
        if (posix_fallocate(fd, 0, static_cast<off_t>(bytes)) != 0 &&
            ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
            std::cerr << "Error: cannot preallocate output " << path << "\n";
            close(fd);
            return false;
        }
        fds[i] = fd;
    }
    return true;
}

static void closeOutputs(std::vector<int>& fds) {
    for (int& fd : fds) {
        if (fd >= 0) close(fd);
        fd = -1;
    }
}

static bool writeFullyAt(int fd, const void* data, uint64_t bytes, uint64_t offset) {
    const uint8_t* cursor = static_cast<const uint8_t*>(data);
    uint64_t completed = 0;
    while (completed < bytes) {
        const ssize_t written = pwrite(
            fd, cursor + completed,
            static_cast<size_t>(bytes - completed),
            static_cast<off_t>(offset + completed)
        );
        if (written > 0) { completed += static_cast<uint64_t>(written); continue; }
        if (written < 0 && errno == EINTR) continue;
        return false;
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
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::cerr
                << "Usage: erwt3d_contest --input DATA.rzfp --output-dir DIR [options]\n\n"
                << "Official competition entrypoint (赛题2 正式入口)\n\n"
                << "  --input PATH           RZFP file (required)\n"
                << "  --output-dir DIR       Output directory (required)\n"
                << "  --random-count N       Random slices per axis (default: 100)\n"
                << "  --continuous-count N   Continuous slices per axis (default: 10)\n"
                << "  --threads N            Thread count (default: 8)\n"
                << "  --memory-limit-mb auto|N    Memory limit MB (default: auto)\n"
                << "  --seed N               Random seed (default: 20260511)\n\n"
                << "Internal policy (fixed):\n"
                << "  strategy = auto, cache = stable-auto, window cache = auto\n"
                << "  execution mode = p5-round (cross-group dedup)\n"
                << "  no active cache drop, no hidden warm-up\n"
                << "  explicit memory limits are enforced\n";
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

    if (!mkdirP(outputDir)) {
        std::cerr << "Error: cannot create output directory " << outputDir << "\n";
        return 1;
    }

    erwt3d::RzfpReader reader(inputPath);
    if (!reader.ok()) {
        std::cerr << "Error: cannot open RZFP file " << inputPath << "\n";
        return 1;
    }
    const auto& header = reader.header();

    (void)reader.ensureDeviceProfile();

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

    std::cout
        << "============================================================\n"
        << "  ERWT3D Contest Entry (正式入口)\n"
        << "============================================================\n"
        << "  File:          " << inputPath << "\n"
        << "  Dims:          " << header.nx << " x "
        << header.ny << " x " << header.nz << "\n"
        << "  Device:        " << std::fixed << std::setprecision(1)
        << reader.deviceProfile().sequential_mb_s << " MB/s\n"
        << "  Memory limit:  " << budget.total_bytes / MiB << " MiB\n"
        << "  Window cache:  " << budget.window_cache_bytes / MiB << " MiB\n"
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

    erwt3d::RzfpReaderConfig config;
    config.strategy = erwt3d::RzfpReadStrategy::Auto;
    config.decode_threads = threads;
    config.window_cache = windowCache;
    config.window_cache_file_identity = reader.fileIdentity();
    config.use_window_cache = true;
    config.adaptive.auto_calibrate_device = true;
    config.adaptive.cache_policy = erwt3d::CachePolicy::StableAuto;
    config.hdd.read_window_bytes = 512ULL * MiB;
    config.hdd.max_gap_bytes = 8ULL * MiB;

    erwt3d::ContestExecutionProfile execProfile;
    if (!erwt3d::executeContestRound(
            reader, header, execGroups, outputDir, "contest",
            config, budget, &execProfile)) {
        std::cerr << "Error: contest round execution failed\n";
        return 1;
    }

    const double readMs = execProfile.read_time_ms;
    const double writeMs = execProfile.write_time_ms;
    const double totalMs = execProfile.total_time_ms;
    const double compositeMs = totalMs / static_cast<double>(groups.size());

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "Read time:   " << readMs / 1000.0 << " s\n";
    std::cout << "Write time:  " << writeMs / 1000.0 << " s\n";
    std::cout << "Total time:  " << totalMs / 1000.0 << " s\n";
    std::cout << "T_composite: " << compositeMs / 1000.0 << " s\n";

    // Write score CSV
    const std::string scorePath = outputDir + "/contest_score.csv";
    {
        std::ofstream out(scorePath);
        out << "metric,value\n"
            << "input_file," << inputPath << '\n'
            << "dimensions," << header.nx << 'x' << header.ny << 'x' << header.nz << '\n'
            << "storage_ratio," << storageRatio << '\n'
            << "read_time_ms," << readMs << '\n'
            << "write_time_ms," << writeMs << '\n'
            << "total_time_ms," << totalMs << '\n'
            << "T_composite_ms," << compositeMs << '\n';
    }

    std::cout << "\nScore written to " << scorePath << "\n";
    return 0;
}
