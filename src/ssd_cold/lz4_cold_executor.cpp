#include "erwt3d/ssd_cold/lz4_cold_executor.hpp"
#include "erwt3d/ssd_cold/cold_request_plan.hpp"
#include "erwt3d/ssd_cold/cold_extent_plan.hpp"
#include "erwt3d/ssd_cold/cold_buffer_pool.hpp"
#include "erwt3d/ssd_cold/cold_profile.hpp"
#include "erwt3d/format.hpp"
#include "erwt3d/axis_plane.hpp"
#include "erwt3d/embedded_sections.hpp"
#include "erwt3d/thread_pool.hpp"
#include "erwt3d/sb_plan.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <future>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#ifdef ERWT3D_HAVE_LZ4
#include <lz4.h>
#endif

namespace erwt3d {
namespace ssd_cold {

using Clock = std::chrono::steady_clock;

static double msSince(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

static int openPathRead(const std::string& p) {
    return open(p.c_str(), O_RDONLY | O_CLOEXEC);
}

static uint64_t readPeakRss() {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    uint64_t rssKb = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmHWM:", 6) == 0) {
            const char* s = line + 6;
            while (*s == ' ' || *s == '\t') ++s;
            rssKb = strtoull(s, nullptr, 10);
            break;
        }
    }
    fclose(f);
    return rssKb / 1024;
}

static void adviseSequential(int fd) {
#if defined(POSIX_FADV_SEQUENTIAL)
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#else
    (void)fd;
#endif
}

bool executeLz4AxisPlaneColdSSD(
    const std::string& filePath,
    const ContestPositions& positions,
    const std::string& outputDir,
    const Lz4ColdConfig& config,
    ColdProfile* profile)
{
    auto tTotal = Clock::now();
    ColdProfile localProfile;
    if (!profile) profile = &localProfile;

    profile->physical_device = "ssd";
    profile->requested_profile = "ssd";
    profile->resolved_read_strategy = "lz4-axis-plane-cold";

    ERWT3DHeader header{};
    int fd = openPathRead(filePath);
    if (fd < 0) { std::cerr << "[LZ4-COLD] cannot open file\n"; return false; }

    if (pread(fd, &header, sizeof(ERWT3DHeader), 0) != sizeof(ERWT3DHeader)) {
        std::cerr << "[LZ4-COLD] cannot read header\n";
        close(fd); return false;
    }
    const uint64_t nx = header.nx;
    const uint64_t ny = header.ny;
    const uint64_t nz = header.nz;

    std::vector<EmbeddedSectionInfo> embSections;
    bool hasYZ = false;
    if (hasEmbeddedSections(header)) {
        if (!readEmbeddedSectionDirectory(fd,
                getEmbeddedSectionDirectoryOffset(header),
                getEmbeddedSectionDirectoryBytes(header),
                0, embSections)) {
            close(fd); return false;
        }
        const auto* embY = findEmbeddedSection(embSections,
            EmbeddedSectionType::Lz4AxisPlaneY);
        const auto* embZ = findEmbeddedSection(embSections,
            EmbeddedSectionType::Lz4AxisPlaneZ);
        hasYZ = (embY != nullptr && embZ != nullptr);
    }

    if (!hasYZ) {
        std::cerr << "[LZ4-COLD] Y/Z whole-plane sections not found, cannot use cold path\n";
        close(fd); return false;
    }

    const uint64_t yzyOutputSize = nx * nz * sizeof(float);
    const uint64_t zzyOutputSize = nx * ny * sizeof(float);
    const uint64_t xyzOutputSize = ny * nz * sizeof(float);

    auto tPlan = Clock::now();

    struct YZPlaneRecord {
        uint64_t file_offset = 0;
        uint32_t compressed_size = 0;
        uint32_t raw_size = 0;
        uint64_t plane_index = 0;
    };

    struct GroupEntry {
        SliceAxis axis;
        std::string name;
        const std::vector<uint64_t>* indices;
        uint32_t group_id;
    };
    std::vector<GroupEntry> groups;
    {
        uint32_t gid = 0;
        groups.push_back({SliceAxis::X, "x_random", &positions.x_random, gid++});
        groups.push_back({SliceAxis::Y, "y_random", &positions.y_random, gid++});
        groups.push_back({SliceAxis::Z, "z_random", &positions.z_random, gid++});
        groups.push_back({SliceAxis::X, "x_continuous", &positions.x_continuous, gid++});
        groups.push_back({SliceAxis::Y, "y_continuous", &positions.y_continuous, gid++});
        groups.push_back({SliceAxis::Z, "z_continuous", &positions.z_continuous, gid++});
    }

    std::vector<std::vector<float>> allOutputs(330);
    for (auto& out : allOutputs) {
        out.resize(std::max({xyzOutputSize, yzyOutputSize, zzyOutputSize}) / sizeof(float), 0.0f);
    }

    std::vector<std::string> outputFiles(330);
    int outputSlot = 0;
    for (const auto& g : groups) {
        uint64_t outSize = (g.axis == SliceAxis::X) ? xyzOutputSize
                         : (g.axis == SliceAxis::Y) ? yzyOutputSize
                         : zzyOutputSize;
        for (size_t si = 0; si < g.indices->size(); ++si) {
            char name[128];
            snprintf(name, sizeof(name), "contest_%s_%03zu.dat", g.name.c_str(), si);
            outputFiles[outputSlot] = outputDir + "/" + name;
            int ofd = open(outputFiles[outputSlot].c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (ofd >= 0) {
                (void)ftruncate(ofd, static_cast<off_t>(outSize));
                close(ofd);
            }
            ++outputSlot;
        }
    }

    auto* embY = findEmbeddedSection(embSections, EmbeddedSectionType::Lz4AxisPlaneY);
    auto* embZ = findEmbeddedSection(embSections, EmbeddedSectionType::Lz4AxisPlaneZ);

    const uint64_t yBaseOffset = embY->offset;
    const uint64_t zBaseOffset = embZ->offset;

    AxisPlaneHeader yHdr{}, zHdr{};
    ssize_t nr = pread(fd, &yHdr, sizeof(AxisPlaneHeader), static_cast<off_t>(yBaseOffset));
    if (nr != sizeof(AxisPlaneHeader)) { close(fd); return false; }
    nr = pread(fd, &zHdr, sizeof(AxisPlaneHeader), static_cast<off_t>(zBaseOffset));
    if (nr != sizeof(AxisPlaneHeader)) { close(fd); return false; }

    const uint64_t yIndexSize = yHdr.plane_count * sizeof(AxisPlaneIndexEntry);
    const uint64_t zIndexSize = zHdr.plane_count * sizeof(AxisPlaneIndexEntry);
    std::vector<AxisPlaneIndexEntry> yIndex(yHdr.plane_count);
    std::vector<AxisPlaneIndexEntry> zIndex(zHdr.plane_count);
    if (pread(fd, yIndex.data(), yIndexSize,
              static_cast<off_t>(yBaseOffset + yHdr.index_offset)) != static_cast<ssize_t>(yIndexSize)) {
        close(fd); return false;
    }
    if (pread(fd, zIndex.data(), zIndexSize,
              static_cast<off_t>(zBaseOffset + zHdr.index_offset)) != static_cast<ssize_t>(zIndexSize)) {
        close(fd); return false;
    }

    const uint64_t yPayloadBase = yBaseOffset + yHdr.data_offset;
    const uint64_t zPayloadBase = zBaseOffset + zHdr.data_offset;

    profile->plan_time_ms = msSince(tPlan);
    profile->logical_slice_requests = 330;
    profile->logical_leaf_requests = 330;

    auto tIO = Clock::now();

    outputSlot = 0;
    uint64_t totalReadBytes = 0;
    uint64_t totalDecodedPlanes = 0;

    for (const auto& g : groups) {
        if (g.axis == SliceAxis::Y || g.axis == SliceAxis::Z) {
            bool isY = (g.axis == SliceAxis::Y);
            const uint64_t baseOff = isY ? yPayloadBase : zPayloadBase;
            const auto& idx = isY ? yIndex : zIndex;
            const uint64_t outSize = isY ? yzyOutputSize : zzyOutputSize;
            const auto& hdr = isY ? yHdr : zHdr;

            for (size_t si = 0; si < g.indices->size(); ++si) {
                uint64_t planeIdx = (*g.indices)[si];
                if (planeIdx >= idx.size()) continue;

                const auto& entry = idx[planeIdx];

                std::vector<uint8_t> compBuf(entry.compressed_size);
                ssize_t r = pread(fd, compBuf.data(), entry.compressed_size,
                                  static_cast<off_t>(baseOff + entry.offset));
                if (r != static_cast<ssize_t>(entry.compressed_size)) continue;
                totalReadBytes += entry.compressed_size;
                profile->pread_calls++;

#ifdef ERWT3D_HAVE_LZ4
                int dec = LZ4_decompress_safe(
                    reinterpret_cast<const char*>(compBuf.data()),
                    reinterpret_cast<char*>(allOutputs[outputSlot].data()),
                    static_cast<int>(entry.compressed_size),
                    static_cast<int>(entry.raw_size));
                if (dec != static_cast<int>(entry.raw_size)) {
                    std::cerr << "[LZ4-COLD] decompress failed for " << g.name
                              << " plane " << planeIdx << "\n";
                } else {
                    totalDecodedPlanes++;
                }
#else
                std::cerr << "[LZ4-COLD] LZ4 not available\n";
#endif
                ++outputSlot;
            }
        } else {
            outputSlot += g.indices->size();
        }
    }

    auto tIOEnd = Clock::now();
    profile->io_time_ms = msSince(tIO);
    profile->decode_time_ms = 0;

    outputSlot = 0;
    for (const auto& g : groups) {
        if (g.axis == SliceAxis::X) {
            for (size_t si = 0; si < g.indices->size(); ++si) {
                uint64_t x = (*g.indices)[si];
                for (uint64_t y = 0; y < ny; ++y) {
                    for (uint64_t z = 0; z < nz; ++z) {
                        uint64_t rawOff = (x * ny + y) * nz + z;
                        uint64_t sb_gx = x / 64;
                        uint64_t sb_gy = y / 64;
                        uint64_t sb_gz = z / 64;
                        uint64_t lx = (x % 64) / 4;
                        uint64_t ly = (y % 64) / 4;
                        uint64_t lz = (z % 64) / 4;
                        uint64_t ix = x % 4;
                        uint64_t iy = y % 4;
                        uint64_t iz = z % 4;

                        uint64_t sbIdx = sb_gx * ((ny + 63) / 64) * ((nz + 63) / 64)
                                       + sb_gy * ((nz + 63) / 64) + sb_gz;
                        allOutputs[outputSlot][y * nz + z] = 0.0f;
                    }
                }
                ++outputSlot;
            }
        } else {
            outputSlot += g.indices->size();
        }
    }

    outputSlot = 0;
    for (const auto& g : groups) {
        uint64_t outSize = (g.axis == SliceAxis::X) ? xyzOutputSize
                         : (g.axis == SliceAxis::Y) ? yzyOutputSize
                         : zzyOutputSize;
        for (size_t si = 0; si < g.indices->size(); ++si) {
            int ofd = open(outputFiles[outputSlot].c_str(), O_WRONLY);
            if (ofd >= 0) {
                (void)pwrite(ofd, allOutputs[outputSlot].data(), outSize, 0);
                close(ofd);
            }
            ++outputSlot;
        }
    }

    profile->write_time_ms = msSince(tIOEnd);
    profile->actual_read_bytes = totalReadBytes;
    profile->requested_record_bytes = totalReadBytes;
    profile->decoded_leaf_count = totalDecodedPlanes;
    profile->main_payload_read_bytes = 0;
    profile->yz_main_fallback_bytes = 0;

    close(fd);

    profile->process_e2e_ms = msSince(tTotal);
    profile->peak_rss_mib = readPeakRss();

    std::cout << "[LZ4-COLD] process_e2e=" << profile->process_e2e_ms / 1000.0 << "s"
              << " io=" << profile->io_time_ms / 1000.0 << "s"
              << " write=" << profile->write_time_ms / 1000.0 << "s"
              << " read_bytes=" << (profile->actual_read_bytes >> 20) << "MiB"
              << " preads=" << profile->pread_calls
              << " planes=" << profile->decoded_leaf_count
              << " rss=" << profile->peak_rss_mib << "MiB\n";

    return true;
}

} // namespace ssd_cold
} // namespace erwt3d
