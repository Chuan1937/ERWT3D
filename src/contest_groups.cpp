#include "erwt3d/contest_groups.hpp"

#include <algorithm>
#include <cerrno>
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

struct InternalGroup {
    SliceAxis axis;
    bool isRandom;
    std::string name;
    const std::vector<uint64_t>* indices;
    int timingIndex;
};

static bool createOutputFiles(
    const InternalGroup& group,
    const std::string& outputDir,
    uint64_t outBytes,
    std::vector<int>& fds
) {
    fds.resize(group.indices->size(), -1);
    for (size_t i = 0; i < group.indices->size(); ++i) {
        std::ostringstream oss;
        oss << outputDir << "/contest_" << group.name << "_"
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
    return true;
}

static void closeFilesSilent(std::vector<int>& fds) {
    for (int& f : fds) {
        if (f >= 0) { close(f); f = -1; }
    }
    fds.clear();
}

static bool closeFilesChecked(std::vector<int>& fds, std::string* error = nullptr) {
    bool allOk = true;
    for (int& f : fds) {
        if (f >= 0) {
            if (close(f) != 0) {
                if (error && allOk) {
                    *error = "close failed for fd " + std::to_string(f) +
                             ": " + std::string(std::strerror(errno));
                }
                allOk = false;
            }
            f = -1;
        }
    }
    fds.clear();
    return allOk;
}

static void finishProfile(ContestUnifiedProfile& p) {
    p.t_x_ms = (p.x_random.time_ms + p.x_continuous.time_ms) / 2.0;
    p.t_y_ms = (p.y_random.time_ms + p.y_continuous.time_ms) / 2.0;
    p.t_z_ms = (p.z_random.time_ms + p.z_continuous.time_ms) / 2.0;
    p.t_composite_ms = (p.t_x_ms + p.t_y_ms + p.t_z_ms) / 3.0;
    p.output_file_count = p.x_random.slice_count + p.y_random.slice_count +
        p.z_random.slice_count + p.x_continuous.slice_count +
        p.y_continuous.slice_count + p.z_continuous.slice_count;
    p.output_total_bytes = p.x_random.total_bytes + p.y_random.total_bytes +
        p.z_random.total_bytes + p.x_continuous.total_bytes +
        p.y_continuous.total_bytes + p.z_continuous.total_bytes;
}

}

bool executeContestGroups(
    const ContestPositions& positions,
    const std::string& outputDir,
    uint64_t nx, uint64_t ny, uint64_t nz,
    const ContestReadBatchFunction& reader,
    ContestUnifiedProfile* profile
) {
    ContestUnifiedProfile p;

    struct GroupDef {
        SliceAxis axis;
        bool isRandom;
        const std::vector<uint64_t>* indices;
    };

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
        const auto& indices = *groups[g].indices;
        if (indices.empty()) {
            *timings[g] = {};
            continue;
        }

        const uint64_t elements = sliceElements(nx, ny, nz, groups[g].axis);
        const uint64_t outBytes = elements * sizeof(float);
        const std::string name = groupName(groups[g].axis, groups[g].isRandom);

        const auto groupStart = Clock::now();

        std::vector<int> fds;
        if (!createOutputFiles({groups[g].axis, groups[g].isRandom, name, &indices, g},
                               outputDir, outBytes, fds)) {
            return false;
        }
        const double createMs = msSince(groupStart);

        std::vector<std::vector<float>> outputs(indices.size());
        for (auto& o : outputs) o.resize(elements);

        const auto readStart = Clock::now();
        if (!reader(groups[g].axis, indices, outputs)) {
            closeFilesSilent(fds);
            return false;
        }
        const double readMs = msSince(readStart);

        const auto writeStart = Clock::now();
        for (size_t i = 0; i < indices.size(); ++i) {
            if (!writeFullyAt(fds[i], outputs[i].data(), outBytes, 0)) {
                closeFilesChecked(fds);
                return false;
            }
        }
        if (!closeFilesChecked(fds)) {
            return false;
        }
        const double writeMs = msSince(writeStart);

        timings[g]->time_ms = msSince(groupStart);
        timings[g]->read_ms = readMs;
        timings[g]->write_ms = writeMs;
        timings[g]->create_files_ms = createMs;
        timings[g]->slice_count = indices.size();
        timings[g]->total_bytes = indices.size() * outBytes;
    }

    p.process_e2e_ms = msSince(e2eStart);
    finishProfile(p);

    if (profile) *profile = p;
    return true;
}

bool executeContestGroupsMerged(
    const ContestPositions& positions,
    const std::string& outputDir,
    uint64_t nx, uint64_t ny, uint64_t nz,
    const MultiGroupReadFunction& mergedReader,
    ContestUnifiedProfile* profile
) {
    ContestUnifiedProfile p;

    InternalGroup groups[6] = {
        {SliceAxis::X, true,  "x_random",      &positions.x_random,      0},
        {SliceAxis::Y, true,  "y_random",      &positions.y_random,      1},
        {SliceAxis::Z, true,  "z_random",      &positions.z_random,      2},
        {SliceAxis::X, false, "x_continuous",  &positions.x_continuous,  3},
        {SliceAxis::Y, false, "y_continuous",  &positions.y_continuous,  4},
        {SliceAxis::Z, false, "z_continuous",  &positions.z_continuous,  5},
    };

    ContestGroupTiming* timings[6] = {
        &p.x_random, &p.y_random, &p.z_random,
        &p.x_continuous, &p.y_continuous, &p.z_continuous,
    };

    const auto e2eStart = Clock::now();

    // Phase 1: Create all output files and pre-0allocate
    const auto createStart = Clock::now();
    std::vector<std::vector<int>> allFds(6);
    std::vector<uint64_t> outBytesPerGroup(6, 0);

    for (int g = 0; g < 6; ++g) {
        if (groups[g].indices->empty()) {
            *timings[g] = {};
            continue;
        }
        const uint64_t elements = sliceElements(nx, ny, nz, groups[g].axis);
        outBytesPerGroup[g] = elements * sizeof(float);
        if (!createOutputFiles(groups[g], outputDir, outBytesPerGroup[g], allFds[g])) {
            for (auto& fds : allFds) closeFilesSilent(fds);
            return false;
        }
    }
    const double totalCreateMs = msSince(createStart);

    // Phase 2: Merge-read all groups at once (cross-group dedup)
    const auto readStart = Clock::now();
    std::vector<GroupReadEntry> readEntries;
    std::vector<std::vector<std::vector<float>>> allOutputs(6);
    for (int g = 0; g < 6; ++g) {
        if (groups[g].indices->empty()) continue;
        const uint64_t elements = sliceElements(nx, ny, nz, groups[g].axis);
        allOutputs[g].resize(groups[g].indices->size());
        for (auto& o : allOutputs[g]) o.resize(elements);

        GroupReadEntry entry;
        entry.original_group_id = static_cast<size_t>(g);
        entry.axis = groups[g].axis;
        entry.isRandom = groups[g].isRandom;
        entry.name = groups[g].name;
        entry.indices = *groups[g].indices;
        readEntries.push_back(std::move(entry));
    }

    if (!mergedReader(readEntries, allOutputs)) {
        for (auto& fds : allFds) closeFilesSilent(fds);
        return false;
    }
    const double readMs = msSince(readStart);

    uint64_t totalSlices = 0;
    for (int g = 0; g < 6; ++g) totalSlices += groups[g].indices->size();

    // Phase 3: Write each group's output and close files
    const auto writeStartAll = Clock::now();
    for (int g = 0; g < 6; ++g) {
        if (groups[g].indices->empty()) continue;

        const auto writeStart = Clock::now();

        const uint64_t outBytes = outBytesPerGroup[g];
        for (size_t i = 0; i < groups[g].indices->size(); ++i) {
            if (!writeFullyAt(allFds[g][i], allOutputs[g][i].data(), outBytes, 0)) {
                for (auto& fds : allFds) closeFilesSilent(fds);
                return false;
            }
        }

        if (!closeFilesChecked(allFds[g])) {
            for (size_t gg = g + 1; gg < 6; ++gg) {
                closeFilesSilent(allFds[gg]);
            }
            return false;
        }

        double writeCloseMs = msSince(writeStart);
        double groupReadMs = (totalSlices > 0)
            ? readMs * static_cast<double>(groups[g].indices->size()) / static_cast<double>(totalSlices)
            : 0.0;

        timings[g]->time_ms = groupReadMs + writeCloseMs;
        timings[g]->read_ms = groupReadMs;
        timings[g]->write_ms = writeCloseMs;
        timings[g]->create_files_ms = 0.0;
        timings[g]->slice_count = groups[g].indices->size();
        timings[g]->total_bytes = groups[g].indices->size() * outBytes;
    }
    const double totalWriteMs = msSince(writeStartAll);

    p.process_e2e_ms = msSince(e2eStart);
    p.merged_read_ms = readMs;
    p.total_write_ms = totalWriteMs;
    p.total_create_files_ms = totalCreateMs;

    p.t_composite_ms = p.process_e2e_ms / 6.0;
    p.t_x_ms = (p.x_random.time_ms + p.x_continuous.time_ms) / 2.0;
    p.t_y_ms = (p.y_random.time_ms + p.y_continuous.time_ms) / 2.0;
    p.t_z_ms = (p.z_random.time_ms + p.z_continuous.time_ms) / 2.0;

    p.output_file_count = p.x_random.slice_count + p.y_random.slice_count +
        p.z_random.slice_count + p.x_continuous.slice_count +
        p.y_continuous.slice_count + p.z_continuous.slice_count;
    p.output_total_bytes = p.x_random.total_bytes + p.y_random.total_bytes +
        p.z_random.total_bytes + p.x_continuous.total_bytes +
        p.y_continuous.total_bytes + p.z_continuous.total_bytes;

    if (profile) *profile = p;
    return true;
}

} // namespace erwt3d
