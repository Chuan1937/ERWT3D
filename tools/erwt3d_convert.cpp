#include "erwt3d/auto_plan.hpp"
#include "erwt3d/writer.hpp"
#include "erwt3d/reader.hpp"
#include "erwt3d/rzfp_writer.hpp"
#include "erwt3d/rzfp_reader.hpp"
#include "erwt3d/lz4_xp_sidecar.hpp"
#include "erwt3d/raw_x_aux.hpp"
#include "erwt3d/memory_budget.hpp"
#include "erwt3d/file_format_detect.hpp"
#include "erwt3d/lz4_axis_plane_writer.hpp"
#include "erwt3d/rzfp_axis_leaf.hpp"
#include "erwt3d/embedded_sections.hpp"
#include "erwt3d/io_profile.hpp"
#include "erwt3d/unified_read_config.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <fcntl.h>
#if defined(__linux__)
#include <sched.h>
#endif
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;
constexpr uint64_t GiB = 1024ULL * 1024ULL * 1024ULL;
constexpr double StorageBudget = 1.50;

double secondsSince(const Clock::time_point& start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

int availableLogicalCpus() {
#if defined(__linux__)
    cpu_set_t mask;
    CPU_ZERO(&mask);
    if (sched_getaffinity(0, sizeof(mask), &mask) == 0) {
        const int count = CPU_COUNT(&mask);
        if (count > 0) return count;
    }
#endif
    const unsigned count = std::thread::hardware_concurrency();
    return count == 0 ? 1 : static_cast<int>(count);
}

int availablePhysicalCores() {
#if defined(__linux__)
    cpu_set_t mask;
    CPU_ZERO(&mask);
    const bool haveAffinity =
        sched_getaffinity(0, sizeof(mask), &mask) == 0;
    std::set<std::pair<int, int>> cores;
    const int logical = availableLogicalCpus();
    const int scanLimit = std::max(logical * 4, 256);
    for (int cpu = 0; cpu < scanLimit; ++cpu) {
        if (haveAffinity && !CPU_ISSET(cpu, &mask)) continue;
        std::ifstream coreFile(
            "/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
            "/topology/core_id");
        std::ifstream packageFile(
            "/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
            "/topology/physical_package_id");
        int core = -1;
        int package = 0;
        if (coreFile >> core) {
            if (!(packageFile >> package)) package = 0;
            cores.emplace(package, core);
        }
    }
    if (!cores.empty()) return static_cast<int>(cores.size());
#endif
    const int fallbackLogical = availableLogicalCpus();
    return fallbackLogical >= 4
        ? std::max(1, fallbackLogical / 2)
        : fallbackLogical;
}

int resolveConversionThreads(uint64_t inputBytes, uint64_t memoryMib) {
    const int physical = availablePhysicalCores();
    int scaleCap = 4;
    if (inputBytes >= 8ULL * GiB) scaleCap = 8;
    if (inputBytes >= 32ULL * GiB) scaleCap = 16;
    if (inputBytes >= 64ULL * GiB) scaleCap = 32;

    // Compression workers use private buffers. Keep at least 128 MiB of the
    // configured budget available per worker even on very small-memory runs.
    const int memoryCap = static_cast<int>(std::max<uint64_t>(
        1, std::min<uint64_t>(32, memoryMib / 128)));
    return std::max(
        1,
        std::min({physical, scaleCap, memoryCap, 32}));
}

uint64_t fileSizeOrZero(const std::string& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return 0;
    return std::filesystem::file_size(path, ec);
}

void removeIfPresent(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

void removeAuxiliaryFiles(const std::string& path) {
    removeIfPresent(path + ".xp");
    removeIfPresent(path + ".yp");
    removeIfPresent(path + ".zp");
    removeIfPresent(erwt3d::rzfpAxisLeafPath(path, erwt3d::PlaneAxis::X));
    removeIfPresent(erwt3d::rzfpAxisLeafPath(path, erwt3d::PlaneAxis::Y));
    removeIfPresent(erwt3d::rzfpAxisLeafPath(path, erwt3d::PlaneAxis::Z));
}

class TemporaryRawStage {
public:
    TemporaryRawStage() = default;
    ~TemporaryRawStage() {
        if (!path_.empty()) removeIfPresent(path_);
    }

    TemporaryRawStage(const TemporaryRawStage&) = delete;
    TemporaryRawStage& operator=(const TemporaryRawStage&) = delete;

    bool active() const { return !path_.empty(); }
    const std::string& path() const { return path_; }
    void setPath(std::string path) { path_ = std::move(path); }

private:
    std::string path_;
};

bool writeAllFd(int fd, const char* data, size_t bytes) {
    size_t done = 0;
    while (done < bytes) {
        const ssize_t n = write(fd, data + done, bytes - done);
        if (n > 0) {
            done += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool stageInputForHdd(
    const std::string& inputPath,
    uint64_t inputSize,
    uint64_t memoryLimitMiB,
    uint64_t maxStageBytes,
    uint64_t additionalWorkingMiB,
    const char* label,
    TemporaryRawStage& stage)
{
#if !defined(__linux__)
    (void)inputPath;
    (void)inputSize;
    (void)memoryLimitMiB;
    (void)maxStageBytes;
    (void)additionalWorkingMiB;
    (void)label;
    (void)stage;
    return false;
#else
    constexpr uint64_t MiB = 1ULL << 20;
    constexpr uint64_t MinStageBytes = 1ULL << 30;
    constexpr uint64_t TmpfsHeadroomBytes = 512ULL << 20;

    if (inputSize < MinStageBytes || inputSize > maxStageBytes) return false;

    const uint64_t inputMiB =
        inputSize / MiB + (inputSize % MiB != 0 ? 1 : 0);
    if (inputMiB >
            std::numeric_limits<uint64_t>::max() - additionalWorkingMiB ||
        memoryLimitMiB < inputMiB + additionalWorkingMiB) {
        std::cout
            << "HDD RAM staging disabled for " << label
            << ": memory budget needs at least "
            << (inputMiB + additionalWorkingMiB)
            << " MiB including conversion working memory\n";
        return false;
    }

    struct statvfs fs{};
    if (statvfs("/dev/shm", &fs) != 0) {
        std::cout
            << "HDD RAM staging disabled: cannot inspect /dev/shm: "
            << std::strerror(errno) << "\n";
        return false;
    }
    const uint64_t fragmentBytes =
        fs.f_frsize != 0 ? static_cast<uint64_t>(fs.f_frsize)
                         : static_cast<uint64_t>(fs.f_bsize);
    const uint64_t availableBytes =
        fragmentBytes != 0 &&
                static_cast<uint64_t>(fs.f_bavail) >
                    std::numeric_limits<uint64_t>::max() / fragmentBytes
            ? std::numeric_limits<uint64_t>::max()
            : static_cast<uint64_t>(fs.f_bavail) * fragmentBytes;
    if (availableBytes < inputSize ||
        availableBytes - inputSize < TmpfsHeadroomBytes) {
        std::cout
            << "HDD RAM staging disabled: /dev/shm has "
            << availableBytes / MiB
            << " MiB free; needs "
            << (inputSize + TmpfsHeadroomBytes) / MiB
            << " MiB\n";
        return false;
    }

    char stageTemplate[] = "/dev/shm/erwt3d-hdd-stage-XXXXXX";
    const int outputFd = mkstemp(stageTemplate);
    if (outputFd < 0) {
        std::cout
            << "HDD RAM staging disabled: mkstemp failed: "
            << std::strerror(errno) << "\n";
        return false;
    }
    const int inputFd = open(inputPath.c_str(), O_RDONLY | O_CLOEXEC);
    if (inputFd < 0) {
        std::cout
            << "HDD RAM staging disabled: cannot open raw input: "
            << std::strerror(errno) << "\n";
        close(outputFd);
        removeIfPresent(stageTemplate);
        return false;
    }

    posix_fadvise(inputFd, 0, 0, POSIX_FADV_SEQUENTIAL);
    constexpr size_t CopyBytes = 16ULL << 20;
    std::vector<char> buffer(CopyBytes);
    uint64_t copied = 0;
    bool ok = true;
    const auto start = Clock::now();
    while (copied < inputSize) {
        const size_t request = static_cast<size_t>(
            std::min<uint64_t>(buffer.size(), inputSize - copied));
        ssize_t n = read(inputFd, buffer.data(), request);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            ok = false;
            break;
        }
        if (!writeAllFd(
                outputFd,
                buffer.data(),
                static_cast<size_t>(n))) {
            ok = false;
            break;
        }
        copied += static_cast<uint64_t>(n);
    }
    close(inputFd);
    if (close(outputFd) != 0) ok = false;

    if (!ok || copied != inputSize) {
        std::cout
            << "HDD RAM staging failed after "
            << copied / MiB
            << " MiB; falling back to direct HDD reads\n";
        removeIfPresent(stageTemplate);
        return false;
    }

    stage.setPath(stageTemplate);
    const double seconds = secondsSince(start);
    std::cout
        << "HDD RAM staging (" << label << "): copied "
        << copied / MiB
        << " MiB once in "
        << seconds
        << "s ("
        << (seconds > 0.0
                ? static_cast<double>(copied) / MiB / seconds
                : 0.0)
        << " MiB/s); subsequent conversion passes use tmpfs\n";
    return true;
#endif
}

bool installPackage(const std::string& workPath, const std::string& outputPath) {
    std::error_code ec;
    std::filesystem::rename(workPath, outputPath, ec);
    if (ec) {
        std::cerr << "Error: cannot install final package: "
                  << ec.message() << "\n";
        return false;
    }
    // Old multi-file conversions with the same output name must not influence
    // discovery or storage accounting after a successful single-file install.
    removeAuxiliaryFiles(outputPath);
    return true;
}

bool hasCanonicalPackageExtension(const std::string& path) {
    static const std::string extension = ".erwt3d";
    return path.size() >= extension.size() &&
           path.compare(
               path.size() - extension.size(),
               extension.size(),
               extension) == 0;
}

void printPackageCopyStats(const erwt3d::EmbeddedPackageStats& stats) {
    constexpr uint64_t MiB = 1024ULL * 1024ULL;
    std::cout
        << "  Package assembly: reflink="
        << stats.reflink_bytes / MiB
        << " MiB, kernel-copy="
        << stats.kernel_copy_bytes / MiB
        << " MiB, buffered-copy="
        << stats.buffered_copy_bytes / MiB
        << " MiB\n";
}

template <typename Header>
bool readHeaderAtStart(const std::string& path, Header& header) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const ssize_t n = pread(fd, &header, sizeof(header), 0);
    close(fd);
    return n == static_cast<ssize_t>(sizeof(header));
}

bool clearExternalXpFlag(const std::string& path) {
    const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    erwt3d::ERWT3DHeader header{};
    bool ok =
        pread(fd, &header, sizeof(header), 0) ==
            static_cast<ssize_t>(sizeof(header)) &&
        erwt3d::validateHeader(header);
    if (ok && erwt3d::hasXPSidecar(header)) {
        header.flags &= ~erwt3d::FLAG_HAS_XP_SIDECAR;
        ok = pwrite(fd, &header, sizeof(header), 0) ==
             static_cast<ssize_t>(sizeof(header));
    }
    close(fd);
    return ok;
}

bool packageExistingOptimizedFile(
    const std::string& inputPath,
    const std::string& outputPath,
    erwt3d::OptimizedFileFormat format)
{
    const std::string workPath = outputPath + ".packing.tmp";
    removeIfPresent(workPath);
    removeAuxiliaryFiles(workPath);

    erwt3d::EmbeddedPackageStats primaryCopyStats;
    if (!erwt3d::copyFileEfficient(
            inputPath,
            workPath,
            &primaryCopyStats)) {
        std::cerr << "Error: cannot create package working file\n";
        return false;
    }
    std::cout << "Fast packaging existing optimized data...\n";
    printPackageCopyStats(primaryCopyStats);

    uint64_t rawBytes = 0;
    bool alreadyEmbedded = false;
    std::vector<erwt3d::EmbeddedSectionInput> sections;

    if (format == erwt3d::OptimizedFileFormat::LZ4_ERWT3D) {
        erwt3d::ERWT3DHeader header{};
        if (!readHeaderAtStart(inputPath, header) ||
            !erwt3d::validateHeader(header) ||
            !clearExternalXpFlag(workPath)) {
            removeIfPresent(workPath);
            return false;
        }
        rawBytes = erwt3d::getRawSize(header);
        alreadyEmbedded = erwt3d::hasEmbeddedSections(header);
        if (!alreadyEmbedded) {
            for (const auto axis : {
                     erwt3d::PlaneAxis::Y,
                     erwt3d::PlaneAxis::Z}) {
                const std::string path =
                    erwt3d::axisPlaneSidecarPath(inputPath, axis);
                if (!std::filesystem::is_regular_file(path)) continue;
                sections.push_back({
                    axis == erwt3d::PlaneAxis::Y
                        ? erwt3d::EmbeddedSectionType::Lz4AxisPlaneY
                        : erwt3d::EmbeddedSectionType::Lz4AxisPlaneZ,
                    path,
                });
            }
        }
    } else if (format == erwt3d::OptimizedFileFormat::RZFP) {
        erwt3d::RzfpFileHeader header{};
        if (!readHeaderAtStart(inputPath, header) ||
            !erwt3d::validateRzfpHeader(header)) {
            removeIfPresent(workPath);
            return false;
        }
        rawBytes = erwt3d::rzfpRawSize(header);
        alreadyEmbedded = erwt3d::hasEmbeddedSections(header);
        if (!alreadyEmbedded &&
            erwt3d::hasRzfpAxisLeaf(header)) {
            for (const auto axis : {
                     erwt3d::PlaneAxis::X,
                     erwt3d::PlaneAxis::Y,
                     erwt3d::PlaneAxis::Z}) {
                const std::string path =
                    erwt3d::rzfpAxisLeafPath(inputPath, axis);
                if (!std::filesystem::is_regular_file(path)) {
                    std::cerr
                        << "Error: missing RZFP axis-leaf file "
                        << path << "\n";
                    removeIfPresent(workPath);
                    return false;
                }
                sections.push_back({
                    static_cast<erwt3d::EmbeddedSectionType>(
                        static_cast<uint32_t>(
                            erwt3d::EmbeddedSectionType::RzfpAxisLeafX) +
                        static_cast<uint32_t>(axis)),
                    path,
                });
            }
        }
    } else {
        removeIfPresent(workPath);
        return false;
    }

    if (!alreadyEmbedded && !sections.empty()) {
        erwt3d::EmbeddedPackageStats packageStats;
        if (!erwt3d::embedSectionsInPlace(
                workPath,
                sections,
                false,
                &packageStats)) {
            std::cerr << "Error: optimized section packaging failed\n";
            removeIfPresent(workPath);
            return false;
        }
        printPackageCopyStats(packageStats);
    } else if (!alreadyEmbedded && sections.empty()) {
        std::cout
            << "Warning: input has no optimized axis sections; "
               "the primary file will be preserved as a single-file package\n";
    }

    const uint64_t packageBytes = fileSizeOrZero(workPath);
    const double ratio =
        rawBytes == 0
            ? 0.0
            : static_cast<double>(packageBytes) /
                  static_cast<double>(rawBytes);
    if (packageBytes == 0 || ratio > StorageBudget) {
        std::cerr
            << "Error: packaged file exceeds storage budget: "
            << ratio << "x\n";
        removeIfPresent(workPath);
        return false;
    }
    if (!installPackage(workPath, outputPath)) {
        removeIfPresent(workPath);
        return false;
    }
    std::cout
        << "Single-file package complete: "
        << outputPath
        << "\n  Storage ratio: "
        << std::fixed << std::setprecision(3)
        << ratio << "x\n";
    return true;
}

}

int main(int argc, char* argv[]) {
    std::string inputPath;
    std::string outputPath;
    uint64_t nx = 0, ny = 0, nz = 0;
    bool toRaw = false;
    int threads = 0;
    bool threadsExplicit = false;
    std::string memoryLimit = "auto";
    std::string ioProfileStr = "auto";
    int axisWorkers = 0;
    int planeWorkers = 0;

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
        } else if (std::strcmp(argv[i], "--output") == 0 || std::strcmp(argv[i], "-o") == 0) {
            outputPath = next();
        } else if (std::strcmp(argv[i], "--nx") == 0) {
            nx = std::stoull(next());
        } else if (std::strcmp(argv[i], "--ny") == 0) {
            ny = std::stoull(next());
        } else if (std::strcmp(argv[i], "--nz") == 0) {
            nz = std::stoull(next());
        } else if (std::strcmp(argv[i], "--threads") == 0) {
            const std::string value = next();
            if (value == "auto") {
                threads = 0;
                threadsExplicit = false;
            } else {
                threads = std::stoi(value);
                threadsExplicit = true;
            }
        } else if (std::strcmp(argv[i], "--to-raw") == 0) {
            toRaw = true;
        } else if (std::strcmp(argv[i], "--memory-limit-mb") == 0) {
            memoryLimit = next();
        } else if (std::strcmp(argv[i], "--io-profile") == 0) {
            ioProfileStr = next();
        } else if (std::strcmp(argv[i], "--axis-workers") == 0) {
            const std::string value = next();
            axisWorkers = value == "auto" ? 0 : std::stoi(value);
        } else if (std::strcmp(argv[i], "--plane-workers") == 0) {
            const std::string value = next();
            planeWorkers = value == "auto" ? 0 : std::stoi(value);
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::cerr
                << "Usage: erwt3d_convert --input data.raw --output data.erwt3d --nx N --ny N --nz N\n\n"
                << "Unified auto converter: samples raw data, estimates LZ4 and RZFP compression,\n"
                << "predicts contest read time, and selects the best format automatically.\n\n"
                << "  --input PATH          Raw float32 or ERWT3D/RZFP file (required)\n"
                << "  --output PATH         Output file (required)\n"
                << "  --nx N                X dimension (required for raw input)\n"
                << "  --ny N                Y dimension (required for raw input)\n"
                << "  --nz N                Z dimension (required for raw input)\n"
                << "  --threads auto|N      Compression threads (default: auto physical cores)\n"
                << "  --memory-limit-mb auto|N  Memory limit in MiB (default: auto)\n"
                << "  --io-profile auto|hdd|ssd|wsl-ssd  Conversion device profile\n"
                << "  --axis-workers auto|1|2|3  RZFP axis repack concurrency\n"
                << "  --plane-workers auto|1|2  LZ4 Y/Z generation concurrency\n"
                << "  --to-raw              Convert ERWT3D/RZFP back to raw float32\n\n"
                << "New optimized files must use the canonical .erwt3d extension.\n"
                << "The internal LZ4/RZFP format is selected automatically and stored in the header.\n\n"
                << "If --input is an existing optimized file, its external axis files are\n"
                << "packaged directly without recompression or axis repacking.\n\n"
                << "Auto-selected candidates:\n"
                << "  A) LZ4 + embedded Y/Z whole-plane sections\n"
                << "  B) RZFP + embedded X/Y/Z axis-leaf sections\n\n"
                << "Internal policy:\n"
                << "  RZFP error: contest_bound=1e-3, internal_bound=7.5e-4\n"
                << "  One self-contained output file; no runtime sidecars\n"
                << "  Storage budget: 1.50x hard limit\n";
            return 0;
        } else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            return 1;
        }
    }

    if (inputPath.empty() || outputPath.empty()) {
        std::cerr << "Error: --input and --output are required\n";
        return 1;
    }

    if (erwt3d::pathsReferToSameFile(inputPath, outputPath)) {
        std::cerr << "Error: input and output refer to the same file\n";
        return 1;
    }

    if (!toRaw && !hasCanonicalPackageExtension(outputPath)) {
        std::cerr
            << "Error: unified optimized output must end with .erwt3d\n"
            << "  requested: " << outputPath << "\n"
            << "  example:   data.erwt3d\n";
        return 1;
    }

    const auto resolvedMemory = erwt3d::resolveMemoryLimit(memoryLimit);
    if (!resolvedMemory.valid) {
        std::cerr << "Error: " << resolvedMemory.error << "\n";
        return 1;
    }
    std::cout << "Memory mode: " << resolvedMemory.mode
              << "\nResolved memory limit: " << resolvedMemory.mib << " MiB\n";

    const uint64_t inputBytes = fileSizeOrZero(inputPath);
    if (!threadsExplicit) {
        threads = resolveConversionThreads(
            inputBytes,
            resolvedMemory.mib);
    }
    if (threads <= 0 || threads > 256) {
        std::cerr << "Error: --threads must be auto or an integer in [1,256]\n";
        return 1;
    }

    if (ioProfileStr != "auto" &&
        ioProfileStr != "hdd" &&
        ioProfileStr != "ssd" &&
        ioProfileStr != "wsl-ssd") {
        std::cerr << "Error: invalid --io-profile " << ioProfileStr << "\n";
        return 1;
    }
    if (axisWorkers < 0 || axisWorkers > 3) {
        std::cerr << "Error: --axis-workers must be auto or an integer in [1,3]\n";
        return 1;
    }
    if (planeWorkers < 0 || planeWorkers > 2) {
        std::cerr << "Error: --plane-workers must be auto or an integer in [1,2]\n";
        return 1;
    }

    std::filesystem::path outputParent =
        std::filesystem::path(outputPath).parent_path();
    if (outputParent.empty()) outputParent = ".";
    erwt3d::UnifiedReadConfig conversionDevice =
        erwt3d::makeUnifiedConfig(
            erwt3d::parseIOProfileType(ioProfileStr),
            outputParent.string(),
            threads,
            resolvedMemory.mib,
            0);
    const bool conversionSSD =
        conversionDevice.io_profile == erwt3d::IOProfileType::SSD ||
        conversionDevice.io_profile == erwt3d::IOProfileType::WSL_SSD;
    const bool conversionRotational =
        erwt3d::detectRotationalFromPath(outputParent.string());
    // HDD staging is a physical-device optimization, not a codec/read-strategy
    // choice. In particular, an SSD explicitly using the HDD large-window
    // read profile must never enter this conversion path.
    const bool hddStagingEnabled =
        !conversionSSD && conversionRotational;
    const int resolvedAxisWorkers =
        axisWorkers > 0
            ? axisWorkers
            : (conversionSSD ? std::min(3, threads) : 1);

    std::cout
        << "Conversion threads: " << threads
        << (threadsExplicit ? " (user)" : " (auto physical-core aware)")
        << "\nConversion IO profile: " << ioProfileStr
        << " -> "
        << erwt3d::ioProfileTypeName(conversionDevice.io_profile)
        << " (" << conversionDevice.resolved_profile_reason << ")"
        << "\nConversion output rotational: "
        << (conversionRotational ? "yes" : "no")
        << "\nHDD RAM staging: "
        << (hddStagingEnabled ? "eligible" : "disabled")
        << "\nRZFP axis workers: " << resolvedAxisWorkers
        << (axisWorkers > 0 ? " (user)" : " (auto)")
        << "\n";

    if (toRaw) {
        auto fmt = erwt3d::detectOptimizedFileFormat(inputPath);

        if (fmt == erwt3d::OptimizedFileFormat::LZ4_ERWT3D) {
            std::cout << "Converting LZ4 ERWT3D to raw...\n";
            erwt3d::ERWT3DReader reader(inputPath);
            if (!reader.readFullToFile(outputPath, threads, resolvedMemory.mib)) {
                std::cerr << "Error: LZ4 reverse conversion failed\n";
                return 1;
            }
        } else if (fmt == erwt3d::OptimizedFileFormat::RZFP) {
            std::cout << "Converting RZFP to raw...\n";
            erwt3d::RzfpReader reader(inputPath);
            if (!reader.ok()) {
                std::cerr << "Error: cannot open RZFP file " << inputPath << "\n";
                return 1;
            }
            erwt3d::RzfpReaderConfig config;
            config.decode_threads = threads;
            if (!reader.readFullToFile(outputPath, config)) {
                std::cerr << "Error: RZFP reverse conversion failed\n";
                return 1;
            }
        } else {
            std::cerr << "Error: unknown file format in " << inputPath
                      << " (expected ERWT3D or RZFP)\n";
            return 1;
        }
        std::cout << "Conversion complete: " << outputPath << "\n";
        return 0;
    }

    const erwt3d::OptimizedFileFormat existingFormat =
        erwt3d::detectOptimizedFileFormat(inputPath);
    if (existingFormat != erwt3d::OptimizedFileFormat::Unknown) {
        if (!packageExistingOptimizedFile(
                inputPath,
                outputPath,
                existingFormat)) {
            return 1;
        }
        return 0;
    }

    if (nx == 0 || ny == 0 || nz == 0) {
        std::cerr << "Error: --nx, --ny, --nz are required for raw input\n";
        return 1;
    }

    uint64_t rawElements = 0;
    uint64_t rawSize = 0;
    if (!erwt3d::checkedMulU64(nx, ny, rawElements) ||
        !erwt3d::checkedMulU64(rawElements, nz, rawElements) ||
        !erwt3d::checkedMulU64(rawElements, sizeof(float), rawSize)) {
        std::cerr << "Error: raw data size overflow\n";
        return 1;
    }

    struct stat st{};
    if (stat(inputPath.c_str(), &st) != 0) {
        std::cerr << "Error: cannot stat input file: " << inputPath << "\n";
        return 1;
    }
    uint64_t fileBytes = static_cast<uint64_t>(st.st_size);
    if (fileBytes != rawSize) {
        std::cerr << "Error: raw file size mismatch:\n"
                  << "  expected=" << rawSize << "\n"
                  << "  actual=" << fileBytes << "\n";
        return 1;
    }

    const auto totalStart = Clock::now();

    std::cout << "============================================================\n"
              << "  ERWT3D Auto Converter\n"
              << "============================================================\n"
              << "  Input:   " << inputPath << "\n"
              << "  Output:  " << outputPath << "\n"
              << "  Dims:    " << nx << " x " << ny << " x " << nz << "\n"
              << "  Raw:     " << rawSize / GiB << " GiB\n"
              << "  Threads: " << threads << "\n"
              << "  Memory:  " << resolvedMemory.mib << " MiB\n"
              << "============================================================\n\n";

    std::cout << "Phase 1: Sampling and planning...\n";
    erwt3d::PlannerWorkload workload;
    erwt3d::PlannerResult plan = erwt3d::planFormat(
        inputPath, nx, ny, nz, threads, StorageBudget, workload);

    if (!plan.recommended.feasible) {
        std::cerr << "Error: no feasible format found: " << plan.recommended.reason << "\n";
        return 1;
    }

    const auto& rec = plan.recommended;
    std::cout << "\nPlan result:\n";
    for (const auto& c : plan.alternatives) {
        std::cout << "  " << c.name
                  << ": ratio=" << std::fixed << std::setprecision(3) << c.total_ratio_mean << "x"
                  << " (upper " << c.total_ratio_upper << "x)"
                  << " T_pred=" << std::setprecision(2) << c.predicted_t_composite << "s"
                  << (c.feasible ? "" : " INFEASIBLE")
                  << (&c == &rec ? " <-- SELECTED" : "")
                  << "\n";
    }
    std::cout << "\nSelected: " << rec.name
              << " (ratio " << rec.total_ratio_mean << "x"
              << ", T_pred " << rec.predicted_t_composite << "s"
              << ", " << rec.reason << ")\n\n";

    const auto planMs = std::chrono::duration<double, std::milli>(Clock::now() - totalStart).count();
    std::cout << "Planning took " << planMs / 1000.0 << "s\n\n";

    std::cout << "Phase 2: Converting...\n";
    const auto convertStart = Clock::now();
    const std::string workPath = outputPath + ".packing.tmp";
    const std::string legacyPath = outputPath + ".legacy.tmp";
    removeIfPresent(workPath);
    removeIfPresent(legacyPath);
    removeAuxiliaryFiles(workPath);

    TemporaryRawStage hddRawStage;
    std::string conversionInputPath = inputPath;
    uint64_t conversionMemoryMiB = resolvedMemory.mib;
    if (rec.main_format == erwt3d::MainFormat::LZ4 &&
        hddStagingEnabled &&
        stageInputForHdd(
            inputPath,
            rawSize,
            resolvedMemory.mib,
            24ULL * GiB,
            rawSize / (1ULL << 20) +
                (rawSize % (1ULL << 20) != 0 ? 1 : 0) +
                2048,
            "LZ4 raw",
            hddRawStage)) {
        conversionInputPath = hddRawStage.path();
        constexpr uint64_t MiB = 1ULL << 20;
        const uint64_t stagedMiB =
            rawSize / MiB + (rawSize % MiB != 0 ? 1 : 0);
        conversionMemoryMiB =
            resolvedMemory.mib > stagedMiB
                ? resolvedMemory.mib - stagedMiB
                : 1;
    }

    if (rec.main_format == erwt3d::MainFormat::LZ4) {
        const auto mainStart = Clock::now();
        erwt3d::RawXAuxStats auxStats;
        if (!erwt3d::writeERWT3DFromFile(
                workPath, conversionInputPath, nx, ny, nz,
                64, 64, 64, 4, 4, 4,
                threads, conversionMemoryMiB,
                0, 0,
                true,
                erwt3d::RawXAuxMode::Off,
                false,
                &auxStats)) {
            std::cerr << "Error: LZ4 conversion failed\n";
            return 1;
        }
        std::cout << "  LZ4 main encode: "
                  << secondsSince(mainStart) << "s\n";

        struct AxisCandidate {
            erwt3d::PlaneAxis axis{};
            erwt3d::EmbeddedSectionType type{};
            std::string path;
            uint64_t bytes = 0;
            double seconds = 0.0;
            bool written = false;
        };

        // Each whole-plane writer holds approximately one raw-volume-sized
        // scatter buffer.  Run Y and Z concurrently only when the configured
        // memory budget can hold both plus working headroom.
        constexpr uint64_t MiB = 1ULL << 20;
        const uint64_t rawMiB =
            rawSize / MiB + (rawSize % MiB != 0 ? 1 : 0);
        const uint64_t parallelPlaneCopies =
            hddRawStage.active() ? 3 : 2;
        const uint64_t parallelPlaneMemoryMiB =
            rawMiB >
                    (std::numeric_limits<uint64_t>::max() - 1024) /
                        parallelPlaneCopies
                ? std::numeric_limits<uint64_t>::max()
                : parallelPlaneCopies * rawMiB + 1024;
        const bool parallelPlaneResources =
            threads >= 2 &&
            resolvedMemory.mib >= parallelPlaneMemoryMiB;
        const bool parallelPlaneEligible =
            conversionSSD && parallelPlaneResources;
        const int resolvedPlaneWorkers =
            planeWorkers > 0
                ? planeWorkers
                : (parallelPlaneEligible ? 2 : 1);
        if (resolvedPlaneWorkers == 2 && !parallelPlaneResources) {
            std::cerr
                << "Error: --plane-workers 2 requires at least two threads "
                   "and "
                << parallelPlaneMemoryMiB
                << " MiB of configured memory for this dataset\n";
            removeIfPresent(workPath);
            return 1;
        }
        std::cout
            << "LZ4 plane workers: " << resolvedPlaneWorkers
            << (planeWorkers > 0 ? " (user)" : " (auto)")
            << "\n";

        const auto buildPlane =
            [&](erwt3d::PlaneAxis axis,
                int workerThreads,
                uint64_t workerMemoryMiB) {
                const auto axisStart = Clock::now();
                AxisCandidate candidate;
                candidate.axis = axis;
                candidate.type =
                    axis == erwt3d::PlaneAxis::Y
                        ? erwt3d::EmbeddedSectionType::Lz4AxisPlaneY
                        : erwt3d::EmbeddedSectionType::Lz4AxisPlaneZ;
                candidate.path =
                    erwt3d::axisPlaneSidecarPath(workPath, axis);
                erwt3d::Lz4AxisPlaneWriterStats axisStats;
                candidate.written =
                    erwt3d::writeLz4AxisPlaneSidecar(
                        conversionInputPath,
                        workPath,
                        axis,
                        nx,
                        ny,
                        nz,
                        std::numeric_limits<uint32_t>::max(),
                        StorageBudget,
                        workerThreads,
                        &axisStats,
                        workerMemoryMiB);
                candidate.seconds = secondsSince(axisStart);
                if (candidate.written) {
                    candidate.bytes = fileSizeOrZero(candidate.path);
                }
                return candidate;
            };

        std::vector<AxisCandidate> candidates;
        if (resolvedPlaneWorkers == 2) {
            const int yThreads = std::max(1, threads / 2);
            const int zThreads = std::max(1, threads - yThreads);
            const uint64_t yMemoryMiB =
                std::max<uint64_t>(1, resolvedMemory.mib / 2);
            const uint64_t zMemoryMiB =
                std::max<uint64_t>(
                    1,
                    resolvedMemory.mib - yMemoryMiB);
            auto yFuture = std::async(
                std::launch::async,
                buildPlane,
                erwt3d::PlaneAxis::Y,
                yThreads,
                yMemoryMiB);
            auto zFuture = std::async(
                std::launch::async,
                buildPlane,
                erwt3d::PlaneAxis::Z,
                zThreads,
                zMemoryMiB);
            candidates.push_back(yFuture.get());
            candidates.push_back(zFuture.get());
        } else {
            for (const auto axis : {
                     erwt3d::PlaneAxis::Y,
                     erwt3d::PlaneAxis::Z}) {
                candidates.push_back(buildPlane(
                    axis,
                    threads,
                    conversionMemoryMiB));
            }
        }
        for (const auto& candidate : candidates) {
            std::cout << "  LZ4 "
                      << erwt3d::axisLabel(candidate.axis)
                      << " section: "
                      << candidate.seconds
                      << "s";
            if (!candidate.written) {
                std::cout
                    << " (skipped: section does not fit its storage gate)";
            }
            std::cout << "\n";
        }

        const uint64_t mainBytes = fileSizeOrZero(workPath);
        uint64_t selectedBytes = mainBytes;
        std::sort(candidates.begin(), candidates.end(),
                  [](const AxisCandidate& a, const AxisCandidate& b) {
                      return a.bytes < b.bytes;
                  });
        std::vector<erwt3d::EmbeddedSectionInput> sections;
        for (const auto& candidate : candidates) {
            if (candidate.written &&
                candidate.bytes != 0 &&
                selectedBytes <= static_cast<uint64_t>(StorageBudget * rawSize) &&
                candidate.bytes <=
                    static_cast<uint64_t>(StorageBudget * rawSize) -
                        selectedBytes) {
                sections.push_back({candidate.type, candidate.path});
                selectedBytes += candidate.bytes;
            } else {
                removeIfPresent(candidate.path);
            }
        }

        erwt3d::EmbeddedPackageStats packageStats;
        const auto packageStart = Clock::now();
        if (!sections.empty() &&
            !erwt3d::embedSectionsInPlace(
                workPath, sections, true, &packageStats)) {
            std::cerr << "Error: cannot create single-file LZ4 package\n";
            removeIfPresent(workPath);
            removeAuxiliaryFiles(workPath);
            return 1;
        }
        if (!sections.empty()) {
            printPackageCopyStats(packageStats);
            std::cout << "  LZ4 package assembly: "
                      << secondsSince(packageStart) << "s\n";
        }

        const uint64_t outBytes = fileSizeOrZero(workPath);
        double storageRatio = rawSize > 0 ? static_cast<double>(outBytes) / rawSize : 0.0;
        if (outBytes == 0 || storageRatio > StorageBudget) {
            std::cerr << "Error: final LZ4 package exceeds storage budget: "
                      << storageRatio << "x\n";
            removeIfPresent(workPath);
            return 1;
        }
        if (!installPackage(workPath, outputPath)) {
            removeIfPresent(workPath);
            return 1;
        }

        std::cout << "LZ4 conversion complete: " << outputPath << "\n"
                  << "  Embedded axes: "
                  << (sections.empty() ? "none" : "")
                  << (std::any_of(
                          sections.begin(), sections.end(),
                          [](const auto& s) {
                              return s.type == erwt3d::EmbeddedSectionType::Lz4AxisPlaneY;
                          }) ? "Y" : "")
                  << (std::any_of(
                          sections.begin(), sections.end(),
                          [](const auto& s) {
                              return s.type == erwt3d::EmbeddedSectionType::Lz4AxisPlaneZ;
                          }) ? "Z" : "")
                  << "\n"
                  << "  Storage ratio: " << std::fixed << std::setprecision(3) << storageRatio << "x\n";

    } else if (rec.main_format == erwt3d::MainFormat::RZFP) {
        const auto encodeStart = Clock::now();
        erwt3d::RzfpWriterConfig cfg;
        cfg.nx = nx;
        cfg.ny = ny;
        cfg.nz = nz;
        cfg.threads = threads;
        cfg.memory_limit_mb = resolvedMemory.mib;
        cfg.codec.error.policy = erwt3d::RelativeErrorPolicy::Strict;
        cfg.codec.error.contest_bound = 1e-3;
        cfg.codec.error.internal_bound = 7.5e-4;
        cfg.physical_order = erwt3d::PhysicalOrder::ZYX;

        erwt3d::RzfpWriterStats stats{};
        if (!erwt3d::writeRzfpFile(inputPath, legacyPath, cfg, &stats)) {
            std::cerr << "Error: RZFP conversion failed\n";
            return 1;
        }
        std::cout << "  RZFP encode: "
                  << secondsSince(encodeStart) << "s\n";

        if (stats.violation_count > 0 ||
            stats.max_relative_error >= 1e-3 ||
            !std::isfinite(stats.max_relative_error)) {
            std::cerr << "Error: RZFP accuracy verification failed:\n"
                      << "  max_relative_error=" << std::setprecision(10)
                      << stats.max_relative_error << "\n"
                      << "  violations=" << stats.violation_count << "\n"
                      << "output removed\n";
            removeIfPresent(legacyPath);
            return 1;
        }

        TemporaryRawStage rzfpLegacyStage;
        std::string repackInputPath = legacyPath;
        uint64_t repackMemoryMiB = resolvedMemory.mib;
        const uint64_t legacyBytes = fileSizeOrZero(legacyPath);
        if (hddStagingEnabled &&
            stageInputForHdd(
                legacyPath,
                legacyBytes,
                resolvedMemory.mib,
                32ULL * GiB,
                8192,
                "RZFP legacy",
                rzfpLegacyStage)) {
            repackInputPath = rzfpLegacyStage.path();
            constexpr uint64_t MiB = 1ULL << 20;
            const uint64_t stagedMiB =
                legacyBytes / MiB +
                (legacyBytes % MiB != 0 ? 1 : 0);
            repackMemoryMiB =
                resolvedMemory.mib > stagedMiB
                    ? resolvedMemory.mib - stagedMiB
                    : 1;
        }

        erwt3d::RzfpAxisLeafRepackStats repackStats;
        const auto repackStart = Clock::now();
        std::cout << "Repacking RZFP into X/Y/Z axis-leaf layout...\n";
        if (!erwt3d::repackRzfpAxisLeaves(
                repackInputPath,
                workPath,
                repackMemoryMiB,
                &repackStats,
                resolvedAxisWorkers)) {
            std::cerr << "Error: RZFP axis-leaf repack failed\n";
            removeIfPresent(legacyPath);
            removeIfPresent(workPath);
            removeAuxiliaryFiles(workPath);
            return 1;
        }
        std::cout << "  RZFP axis repack: "
                  << secondsSince(repackStart) << "s\n";
        removeIfPresent(legacyPath);

        std::vector<erwt3d::EmbeddedSectionInput> sections = {
            {erwt3d::EmbeddedSectionType::RzfpAxisLeafX,
             erwt3d::rzfpAxisLeafPath(workPath, erwt3d::PlaneAxis::X)},
            {erwt3d::EmbeddedSectionType::RzfpAxisLeafY,
             erwt3d::rzfpAxisLeafPath(workPath, erwt3d::PlaneAxis::Y)},
            {erwt3d::EmbeddedSectionType::RzfpAxisLeafZ,
             erwt3d::rzfpAxisLeafPath(workPath, erwt3d::PlaneAxis::Z)},
        };
        erwt3d::EmbeddedPackageStats packageStats;
        const auto packageStart = Clock::now();
        if (!erwt3d::embedSectionsInPlace(
                workPath, sections, true, &packageStats)) {
            std::cerr << "Error: cannot create single-file RZFP package\n";
            removeIfPresent(workPath);
            removeAuxiliaryFiles(workPath);
            return 1;
        }
        printPackageCopyStats(packageStats);
        std::cout << "  RZFP package assembly: "
                  << secondsSince(packageStart) << "s\n";
        const uint64_t outBytes = fileSizeOrZero(workPath);
        const double storageRatio =
            rawSize > 0 ? static_cast<double>(outBytes) / rawSize : 0.0;
        if (outBytes == 0 || storageRatio > StorageBudget) {
            std::cerr << "Error: final RZFP package exceeds storage budget: "
                      << storageRatio << "x\n";
            removeIfPresent(workPath);
            return 1;
        }
        if (!installPackage(workPath, outputPath)) {
            removeIfPresent(workPath);
            return 1;
        }

        std::cout << "RZFP conversion complete: " << outputPath << "\n"
                  << "  Storage ratio: " << std::fixed << std::setprecision(3)
                  << storageRatio << "x\n"
                  << "  Embedded axes: XYZ\n"
                  << "  Max relative error: " << std::setprecision(10)
                  << stats.max_relative_error << "\n"
                  << "  Violations: " << stats.violation_count << "\n";
    } else {
        std::cerr << "Error: unknown format selected: " << static_cast<int>(rec.main_format) << "\n";
        return 1;
    }

    const auto convertMs = std::chrono::duration<double, std::milli>(Clock::now() - convertStart).count();
    const auto totalMs = std::chrono::duration<double, std::milli>(Clock::now() - totalStart).count();
    std::cout << "\nConversion took " << convertMs / 1000.0 << "s\n"
              << "Total (plan+convert) " << totalMs / 1000.0 << "s\n";

    return 0;
}
