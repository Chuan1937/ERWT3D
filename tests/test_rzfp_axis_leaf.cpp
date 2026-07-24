#include "erwt3d/rzfp_axis_leaf.hpp"
#include "erwt3d/rzfp_reader.hpp"
#include "erwt3d/rzfp_writer.hpp"

#include <algorithm>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

float valueAt(uint64_t x, uint64_t y, uint64_t z) {
    if ((x + y + z) % 17 == 0) return 0.0f;
    return static_cast<float>(
        1.0 +
        x * 0.125 +
        y * 0.03125 +
        z * 0.0078125);
}

bool writeRaw(
    const std::string& path,
    uint64_t nx,
    uint64_t ny,
    uint64_t nz
) {
    std::vector<float> data(nx * ny * nz);
    for (uint64_t x = 0; x < nx; ++x) {
        for (uint64_t y = 0; y < ny; ++y) {
            for (uint64_t z = 0; z < nz; ++z) {
                data[(x * ny + y) * nz + z] =
                    valueAt(x, y, z);
            }
        }
    }

    const int fd = open(
        path.c_str(),
        O_WRONLY | O_CREAT | O_TRUNC,
        0644);
    if (fd < 0) return false;
    const uint8_t* bytes =
        reinterpret_cast<const uint8_t*>(
            data.data());
    uint64_t remaining =
        data.size() * sizeof(float);
    while (remaining != 0) {
        const ssize_t n = write(
            fd,
            bytes,
            static_cast<size_t>(remaining));
        if (n <= 0) {
            close(fd);
            return false;
        }
        bytes += n;
        remaining -= static_cast<uint64_t>(n);
    }
    close(fd);
    return true;
}

bool equal(const std::vector<float>& a,
           const std::vector<float>& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin());
}

void removeAxisFiles(const std::string& path) {
    unlink(path.c_str());
    unlink(erwt3d::rzfpAxisLeafPath(
        path, erwt3d::PlaneAxis::X).c_str());
    unlink(erwt3d::rzfpAxisLeafPath(
        path, erwt3d::PlaneAxis::Y).c_str());
    unlink(erwt3d::rzfpAxisLeafPath(
        path, erwt3d::PlaneAxis::Z).c_str());
}

} // namespace

int main() {
    constexpr uint64_t nx = 19;
    constexpr uint64_t ny = 21;
    constexpr uint64_t nz = 23;
    const std::string prefix =
        "/tmp/erwt3d_rzfp_axis_leaf";
    const std::string raw = prefix + ".raw";
    const std::string legacy = prefix + ".rzfp";
    const std::string axis = prefix + ".axis.rzfp";

    unlink(raw.c_str());
    unlink(legacy.c_str());
    removeAxisFiles(axis);

    check(writeRaw(raw, nx, ny, nz), "write raw");

    erwt3d::RzfpWriterConfig writerConfig;
    writerConfig.nx = nx;
    writerConfig.ny = ny;
    writerConfig.nz = nz;
    writerConfig.super_size = 16;
    writerConfig.leaf_size = 4;
    writerConfig.threads = 2;

    erwt3d::RzfpWriterStats writerStats;
    check(
        erwt3d::writeRzfpFile(
            raw,
            legacy,
            writerConfig,
            &writerStats),
        "write legacy RZFP");
    check(
        writerStats.violation_count == 0,
        "legacy violations");

    erwt3d::RzfpAxisLeafRepackStats repackStats;
    check(
        erwt3d::repackRzfpAxisLeaves(
            legacy,
            axis,
            256,
            &repackStats),
        "axis-leaf repack");
    check(
        repackStats.storage_ratio > 0.0 &&
            repackStats.storage_ratio < 20.0,
        "axis-leaf storage accounting");

    erwt3d::RzfpReader legacyReader(legacy);
    erwt3d::RzfpReader axisReader(axis);
    check(legacyReader.ok(), "legacy reader");
    check(axisReader.ok(), "axis reader");

    struct Case {
        erwt3d::SliceAxis axis;
        uint64_t index;
        uint64_t elements;
    };
    const Case cases[] = {
        {erwt3d::SliceAxis::X, 0, ny * nz},
        {erwt3d::SliceAxis::X, nx - 1, ny * nz},
        {erwt3d::SliceAxis::Y, ny / 2, nx * nz},
        {erwt3d::SliceAxis::Z, nz - 1, nx * ny},
    };

    erwt3d::RzfpReaderConfig config;
    config.decode_threads = 2;
    for (const auto& item : cases) {
        std::vector<float> expected(item.elements);
        std::vector<float> actual(item.elements);
        check(
            legacyReader.readSlice(
                item.axis,
                item.index,
                expected.data()),
            "legacy slice");
        check(
            axisReader.readSlicesBatch(
                {{item.axis, item.index, actual.data()}},
                config),
            "axis slice");
        check(equal(expected, actual), "slice equality");
    }

    erwt3d::RzfpReadProfile profile;
    config.profile = &profile;
    std::vector<float> x0(ny * nz);
    std::vector<float> x1(ny * nz);
    check(
        axisReader.readSlicesBatch(
            {
                {erwt3d::SliceAxis::X, 0, x0.data()},
                {erwt3d::SliceAxis::X, 1, x1.data()},
            },
            config),
        "same-slab batch");
    check(
        profile.selected_strategy ==
            erwt3d::RzfpReadStrategy::AxisLeafReplica,
        "axis strategy selected");
    check(profile.pread_calls == 1, "same-slab one pread");

    const std::string missing =
        erwt3d::rzfpAxisLeafPath(
            axis,
            erwt3d::PlaneAxis::Z);
    const std::string hidden = missing + ".hidden";
    check(
        rename(missing.c_str(), hidden.c_str()) == 0,
        "hide sidecar");
    erwt3d::RzfpReader corruptReader(axis);
    check(!corruptReader.ok(), "missing sidecar rejected");
    rename(hidden.c_str(), missing.c_str());

    unlink(raw.c_str());
    unlink(legacy.c_str());
    removeAxisFiles(axis);

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "RZFP axis-leaf tests passed\n";
    return 0;
}
