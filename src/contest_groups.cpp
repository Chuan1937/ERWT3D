#include "erwt3d/contest_groups.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace erwt3d {

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

uint64_t sliceElements(uint64_t nx, uint64_t ny, uint64_t nz, SliceAxis axis) {
    switch (axis) {
        case SliceAxis::X: return ny * nz;
        case SliceAxis::Y: return nx * nz;
        case SliceAxis::Z: return nx * ny;
    }
    return 0;
}

std::string groupName(SliceAxis axis, bool isRandom) {
    const char* a = (axis == SliceAxis::X) ? "x" : (axis == SliceAxis::Y) ? "y" : "z";
    return std::string(a) + "_" + (isRandom ? "random" : "continuous");
}

bool writeFullyAt(int fd, const void* buf, size_t len, off_t offset) {
    const char* p = static_cast<const char*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = pwrite(fd, p + (len - remaining), remaining, offset + static_cast<off_t>(len - remaining));
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

struct GroupDef {
    SliceAxis axis;
    bool isRandom;
    const std::vector<uint64_t>* indices;
};

bool executeOneGroup(
    const GroupDef& group,
    const std::string& outputDir,
    uint64_t nx, uint64_t ny, uint64_t nz,
    const ContestReadBatchFunction& reader,
    ContestGroupTiming& timing
) {
    const auto& indices = *group.indices;
    if (indices.empty()) {
        timing = {};
        return true;
    }

    const uint64_t elements = sliceElements(nx, ny, nz, group.axis);
    const uint64_t outBytes = elements * sizeof(float);
    const std::string name = groupName(group.axis, group.isRandom);

    // Timing starts: before creating first output file
    const auto groupStart = Clock::now();

    // Create and pre-allocate all output files
    std::vector<int> fds(indices.size(), -1);
    for (size_t i = 0; i < indices.size(); ++i) {
        std::ostringstream oss;
        oss << outputDir << "/contest_" << name << "_"
            << std::setfill('0') << std::setw(3) << i << ".dat";
        int fd = open(oss.str().c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            for (int f : fds) if (f >= 0) close(f);
            return false;
        }
        if (ftruncate(fd, static_cast<off_t>(outBytes)) != 0) {
            close(fd);
            for (int f : fds) if (f >= 0) close(f);
            return false;
        }
        fds[i] = fd;
    }

    // Read all slices via the reader callback (single batch for optimal cross-group dedup)
    std::vector<std::vector<float>> outputs(indices.size());
    for (auto& o : outputs) o.resize(elements);

    if (!reader(group.axis, indices, outputs)) {
        for (int f : fds) if (f >= 0) close(f);
        return false;
    }

    // Write all slices
    for (size_t i = 0; i < indices.size(); ++i) {
        if (!writeFullyAt(fds[i], outputs[i].data(), outBytes, 0)) {
            for (int f : fds) if (f >= 0) close(f);
            return false;
        }
    }

    // Close all files
    for (int f : fds) if (f >= 0) close(f);

    // Timing ends: after all files closed
    timing.time_ms = msSince(groupStart);
    timing.slice_count = indices.size();
    timing.total_bytes = indices.size() * outBytes;

    return true;
}

} // namespace

bool executeContestGroups(
    const ContestPositions& positions,
    const std::string& outputDir,
    uint64_t nx, uint64_t ny, uint64_t nz,
    const ContestReadBatchFunction& reader,
    ContestUnifiedProfile* profile
) {
    ContestUnifiedProfile p;

    GroupDef groups[6] = {
        {SliceAxis::X, true,  &positions.x_random},
        {SliceAxis::Y, true,  &positions.y_random},
        {SliceAxis::Z, true,  &positions.z_random},
        {SliceAxis::X, false, &positions.x_continuous},
        {SliceAxis::Y, false, &positions.y_continuous},
        {SliceAxis::Z, false, &positions.z_continuous},
    };

    ContestGroupTiming* timings[6] = {
        &p.x_random, &p.y_random, &p.z_random,
        &p.x_continuous, &p.y_continuous, &p.z_continuous,
    };

    const auto e2eStart = Clock::now();

    for (int g = 0; g < 6; ++g) {
        if (!executeOneGroup(groups[g], outputDir, nx, ny, nz, reader, *timings[g])) {
            return false;
        }
        p.output_file_count += timings[g]->slice_count;
        p.output_total_bytes += timings[g]->total_bytes;
    }

    p.process_e2e_ms = msSince(e2eStart);

    p.t_x_ms = (p.x_random.time_ms + p.x_continuous.time_ms) / 2.0;
    p.t_y_ms = (p.y_random.time_ms + p.y_continuous.time_ms) / 2.0;
    p.t_z_ms = (p.z_random.time_ms + p.z_continuous.time_ms) / 2.0;
    p.t_composite_ms = (p.t_x_ms + p.t_y_ms + p.t_z_ms) / 3.0;

    if (profile) *profile = p;

    return true;
}

} // namespace erwt3d
