#include "erwt3d/axis_plane.hpp"
#include "erwt3d/embedded_sections.hpp"
#include "erwt3d/lz4_axis_plane_writer.hpp"
#include "erwt3d/reader.hpp"
#include "erwt3d/writer.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace {

int failures = 0;

void check(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

float valueAt(uint64_t x, uint64_t y, uint64_t z) {
    return static_cast<float>(x * 10000 + y * 100 + z);
}

} // namespace

int main() {
    constexpr uint64_t nx = 18;
    constexpr uint64_t ny = 20;
    constexpr uint64_t nz = 22;
    const std::string dir = "/tmp/erwt3d_embedded_sections";
    const std::string raw = dir + "/data.raw";
    const std::string package = dir + "/data.erwt3d";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    std::vector<float> input(nx * ny * nz);
    for (uint64_t x = 0; x < nx; ++x) {
        for (uint64_t y = 0; y < ny; ++y) {
            for (uint64_t z = 0; z < nz; ++z) {
                input[(x * ny + y) * nz + z] = valueAt(x, y, z);
            }
        }
    }
    {
        std::ofstream output(raw, std::ios::binary);
        output.write(
            reinterpret_cast<const char*>(input.data()),
            static_cast<std::streamsize>(input.size() * sizeof(float)));
        check(output.good(), "write raw fixture");
    }

    check(
        erwt3d::writeERWT3DFromFile(
            package, raw, nx, ny, nz,
            16, 16, 16, 4, 4, 4,
            2, 256, 0, 0, true,
            erwt3d::RawXAuxMode::Off, false),
        "write LZ4 primary");
    check(
        erwt3d::writeLz4AxisPlaneSidecar(
            raw, package, erwt3d::PlaneAxis::Y,
            nx, ny, nz, 128 * 1024, 100.0, 2),
        "write Y section source");
    check(
        erwt3d::writeLz4AxisPlaneSidecar(
            raw, package, erwt3d::PlaneAxis::Z,
            nx, ny, nz, 128 * 1024, 100.0, 2),
        "write Z section source");

    const std::string yPath =
        erwt3d::axisPlaneSidecarPath(package, erwt3d::PlaneAxis::Y);
    const std::string zPath =
        erwt3d::axisPlaneSidecarPath(package, erwt3d::PlaneAxis::Z);
    erwt3d::EmbeddedPackageStats stats;
    check(
        erwt3d::embedSectionsInPlace(
            package,
            {
                {erwt3d::EmbeddedSectionType::Lz4AxisPlaneY, yPath},
                {erwt3d::EmbeddedSectionType::Lz4AxisPlaneZ, zPath},
            },
            true,
            &stats),
        "embed sidecars");
    check(!std::filesystem::exists(yPath), "Y source removed");
    check(!std::filesystem::exists(zPath), "Z source removed");
    check(
        stats.package_bytes == std::filesystem::file_size(package),
        "package size recorded");
    check(
        stats.reflink_bytes +
                stats.kernel_copy_bytes +
                stats.buffered_copy_bytes ==
            stats.section_bytes,
        "all section bytes use a recorded copy path");

    const std::string copiedPackage =
        dir + "/copied.erwt3d";
    erwt3d::EmbeddedPackageStats copyStats;
    check(
        erwt3d::copyFileEfficient(
            package,
            copiedPackage,
            &copyStats),
        "fast-copy completed package");
    check(
        copyStats.package_bytes ==
            std::filesystem::file_size(copiedPackage),
        "fast-copy size");

    erwt3d::ERWT3DReader reader(copiedPackage);
    check(
        erwt3d::hasEmbeddedSections(reader.getHeader()),
        "embedded flag visible");
    std::vector<float> yPlane(nx * nz);
    std::vector<float> zPlane(nx * ny);
    erwt3d::HDDReadWindowConfig window;
    check(
        reader.readSlicesBatch(
            {
                {erwt3d::SliceAxis::Y, 7, yPlane.data()},
                {erwt3d::SliceAxis::Z, 9, zPlane.data()},
            },
            2, 256, window),
        "read embedded planes");
    for (uint64_t x = 0; x < nx; ++x) {
        for (uint64_t z = 0; z < nz; ++z) {
            check(
                yPlane[x * nz + z] == valueAt(x, 7, z),
                "Y value");
        }
        for (uint64_t y = 0; y < ny; ++y) {
            check(
                zPlane[x * ny + y] == valueAt(x, y, 9),
                "Z value");
        }
    }

    std::filesystem::remove_all(dir);
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "embedded section tests passed\n";
    return 0;
}
