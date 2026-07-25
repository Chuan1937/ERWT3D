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

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;
constexpr uint64_t GiB = 1024ULL * 1024ULL * 1024ULL;
constexpr double StorageBudget = 1.50;

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
    int threads = 8;
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
        } else if (std::strcmp(argv[i], "--output") == 0 || std::strcmp(argv[i], "-o") == 0) {
            outputPath = next();
        } else if (std::strcmp(argv[i], "--nx") == 0) {
            nx = std::stoull(next());
        } else if (std::strcmp(argv[i], "--ny") == 0) {
            ny = std::stoull(next());
        } else if (std::strcmp(argv[i], "--nz") == 0) {
            nz = std::stoull(next());
        } else if (std::strcmp(argv[i], "--threads") == 0) {
            threads = std::stoi(next());
        } else if (std::strcmp(argv[i], "--to-raw") == 0) {
            toRaw = true;
        } else if (std::strcmp(argv[i], "--memory-limit-mb") == 0) {
            memoryLimit = next();
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
                << "  --threads N           Thread count (default: 8)\n"
                << "  --memory-limit-mb auto|N  Memory limit in MiB (default: auto)\n"
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

    if (rec.main_format == erwt3d::MainFormat::LZ4) {
        erwt3d::RawXAuxStats auxStats;
        if (!erwt3d::writeERWT3DFromFile(
                workPath, inputPath, nx, ny, nz,
                64, 64, 64, 4, 4, 4,
                threads, resolvedMemory.mib,
                0, 0,
                true,
                erwt3d::RawXAuxMode::Off,
                false,
                &auxStats)) {
            std::cerr << "Error: LZ4 conversion failed\n";
            return 1;
        }

        struct AxisCandidate {
            erwt3d::PlaneAxis axis{};
            erwt3d::EmbeddedSectionType type{};
            std::string path;
            uint64_t bytes = 0;
        };
        std::vector<AxisCandidate> candidates;
        for (const auto axis : {erwt3d::PlaneAxis::Y, erwt3d::PlaneAxis::Z}) {
            erwt3d::Lz4AxisPlaneWriterStats axisStats;
            std::cout << "Generating LZ4 "
                      << (axis == erwt3d::PlaneAxis::Y ? "Y" : "Z")
                      << " whole-plane section...\n";
            if (!erwt3d::writeLz4AxisPlaneSidecar(
                    inputPath, workPath, axis, nx, ny, nz,
                    128 * 1024, StorageBudget, threads, &axisStats)) {
                std::cout << "  skipped: section does not fit its storage gate\n";
                continue;
            }
            const std::string sidecar =
                erwt3d::axisPlaneSidecarPath(workPath, axis);
            candidates.push_back({
                axis,
                axis == erwt3d::PlaneAxis::Y
                    ? erwt3d::EmbeddedSectionType::Lz4AxisPlaneY
                    : erwt3d::EmbeddedSectionType::Lz4AxisPlaneZ,
                sidecar,
                fileSizeOrZero(sidecar),
            });
        }

        const uint64_t mainBytes = fileSizeOrZero(workPath);
        uint64_t selectedBytes = mainBytes;
        std::sort(candidates.begin(), candidates.end(),
                  [](const AxisCandidate& a, const AxisCandidate& b) {
                      return a.bytes < b.bytes;
                  });
        std::vector<erwt3d::EmbeddedSectionInput> sections;
        for (const auto& candidate : candidates) {
            if (candidate.bytes != 0 &&
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

        erwt3d::RzfpAxisLeafRepackStats repackStats;
        std::cout << "Repacking RZFP into X/Y/Z axis-leaf layout...\n";
        if (!erwt3d::repackRzfpAxisLeaves(
                legacyPath, workPath, resolvedMemory.mib, &repackStats)) {
            std::cerr << "Error: RZFP axis-leaf repack failed\n";
            removeIfPresent(legacyPath);
            removeIfPresent(workPath);
            removeAuxiliaryFiles(workPath);
            return 1;
        }
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
        if (!erwt3d::embedSectionsInPlace(
                workPath, sections, true, &packageStats)) {
            std::cerr << "Error: cannot create single-file RZFP package\n";
            removeIfPresent(workPath);
            removeAuxiliaryFiles(workPath);
            return 1;
        }
        printPackageCopyStats(packageStats);
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
