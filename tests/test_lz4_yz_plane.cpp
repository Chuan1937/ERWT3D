#include "erwt3d/axis_plane.hpp"
#include "erwt3d/lz4_axis_plane_writer.hpp"
#include "erwt3d/reader.hpp"
#include "erwt3d/raw_layout.hpp"
#include "erwt3d/writer.hpp"

#ifdef ERWT3D_HAVE_LZ4
#include <lz4.h>
#endif

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace erwt3d;

namespace {

int testsPassed = 0;
int testsFailed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "FAIL: " << msg << " at line " << __LINE__ << "\n"; \
        ++testsFailed; \
    } else { \
        ++testsPassed; \
    } \
} while (0)

const std::string kTmpDir = "/tmp/test_lz4_axis_plane";
constexpr uint64_t kNx = 40;
constexpr uint64_t kNy = 30;
constexpr uint64_t kNz = 50;
constexpr uint32_t kChunkElements = 200;

std::string rawPath() { return kTmpDir + "/data.raw"; }
std::string lz4Path() { return kTmpDir + "/data.erwt3d"; }

bool readAt(
    const std::string& path,
    uint64_t offset,
    void* buffer,
    size_t bytes
) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    input.seekg(static_cast<std::streamoff>(offset));
    if (!input) return false;
    input.read(static_cast<char*>(buffer), static_cast<std::streamsize>(bytes));
    return input.gcount() == static_cast<std::streamsize>(bytes);
}

void prepareInput() {
    std::filesystem::remove_all(kTmpDir);
    std::filesystem::create_directories(kTmpDir);

    std::vector<float> data(kNx * kNy * kNz);
    for (uint64_t x = 0; x < kNx; ++x) {
        for (uint64_t y = 0; y < kNy; ++y) {
            for (uint64_t z = 0; z < kNz; ++z) {
                data[rawOffsetZFastest(x, y, z, kNy, kNz)] =
                    static_cast<float>(x * 10000 + y * 100 + z);
            }
        }
    }

    std::ofstream raw(rawPath(), std::ios::binary);
    raw.write(
        reinterpret_cast<const char*>(data.data()),
        static_cast<std::streamsize>(data.size() * sizeof(float)));
    CHECK(raw.good(), "write raw fixture");
    raw.close();

    CHECK(
        writeERWT3DFromFile(
            lz4Path(),
            rawPath(),
            kNx,
            kNy,
            kNz,
            64,
            64,
            64,
            4,
            4,
            4,
            2,
            256,
            0,
            0,
            true,
            RawXAuxMode::Off,
            false),
        "write LZ4 main file");
}

bool decompressPlane(
    const std::string& sidecarPath,
    const AxisPlaneHeader& header,
    uint64_t planeIndex,
    std::vector<float>& output
) {
#ifdef ERWT3D_HAVE_LZ4
    const PlaneAxis axis = static_cast<PlaneAxis>(header.axis);
    const AxisPlaneShape shape =
        makeAxisPlaneShape(axis, header.nx, header.ny, header.nz);
    if (planeIndex >= header.plane_count ||
        header.total_chunks !=
            header.plane_count * header.chunks_per_plane ||
        output.size() != shape.plane_elements) {
        return false;
    }

    std::vector<AxisPlaneIndexEntry> index(
        static_cast<size_t>(header.total_chunks));
    if (!readAt(
            sidecarPath,
            header.index_offset,
            index.data(),
            index.size() * sizeof(AxisPlaneIndexEntry))) {
        return false;
    }

    uint64_t outputBytes = 0;
    for (uint32_t chunk = 0; chunk < header.chunks_per_plane; ++chunk) {
        const AxisPlaneIndexEntry& entry =
            index[static_cast<size_t>(
                planeIndex * header.chunks_per_plane + chunk)];
        if (entry.compressed_size == 0 || entry.raw_size == 0 ||
            outputBytes + entry.raw_size > shape.plane_bytes) {
            return false;
        }

        std::vector<char> compressed(entry.compressed_size);
        if (!readAt(
                sidecarPath,
                entry.offset,
                compressed.data(),
                compressed.size())) {
            return false;
        }

        const int decoded = LZ4_decompress_safe(
            compressed.data(),
            reinterpret_cast<char*>(output.data()) + outputBytes,
            static_cast<int>(compressed.size()),
            static_cast<int>(entry.raw_size));
        if (decoded != static_cast<int>(entry.raw_size)) return false;
        outputBytes += entry.raw_size;
    }
    return outputBytes == shape.plane_bytes;
#else
    (void)sidecarPath;
    (void)header;
    (void)planeIndex;
    (void)output;
    return false;
#endif
}

void verifyPlaneValues(
    PlaneAxis axis,
    uint64_t planeIndex,
    const std::vector<float>& plane
) {
    if (axis == PlaneAxis::Y) {
        for (uint64_t x = 0; x < kNx; ++x) {
            for (uint64_t z = 0; z < kNz; ++z) {
                const float expected = static_cast<float>(
                    x * 10000 + planeIndex * 100 + z);
                CHECK(
                    std::abs(plane[x * kNz + z] - expected) < 1e-6f,
                    "Y plane value mismatch");
            }
        }
    } else {
        for (uint64_t x = 0; x < kNx; ++x) {
            for (uint64_t y = 0; y < kNy; ++y) {
                const float expected = static_cast<float>(
                    x * 10000 + y * 100 + planeIndex);
                CHECK(
                    std::abs(plane[x * kNy + y] - expected) < 1e-6f,
                    "Z plane value mismatch");
            }
        }
    }
}

void testRoundTrip(PlaneAxis axis, uint64_t planeIndex) {
    Lz4AxisPlaneWriterStats stats;
    CHECK(
        writeLz4AxisPlaneSidecar(
            rawPath(),
            lz4Path(),
            axis,
            kNx,
            kNy,
            kNz,
            kChunkElements,
            100.0,  // no practical limit for tiny test dataset
            3,
            &stats),
        std::string("write ") + axisLabel(axis) + " sidecar");
    CHECK(stats.written, "writer stats mark output written");
    CHECK(stats.axis == axis, "writer stats axis");

    const std::string sidecarPath = axisPlaneSidecarPath(lz4Path(), axis);
    CHECK(std::filesystem::exists(sidecarPath), "sidecar exists");
    CHECK(
        stats.sidecar_bytes == std::filesystem::file_size(sidecarPath),
        "sidecar byte count includes header, index, and payload");

    AxisPlaneHeader header {};
    CHECK(
        readAt(sidecarPath, 0, &header, sizeof(header)),
        "read sidecar header");
    CHECK(
        validateAxisPlaneHeader(header, kNx, kNy, kNz),
        "validate sidecar header");
    CHECK(
        header.axis == static_cast<uint8_t>(axis),
        "header axis");
    CHECK(
        header.compression == AXISPLANE_COMPRESSION_LZ4,
        "header compression");
    CHECK(header.chunks_per_plane > 1, "exercise multi-chunk layout");
    CHECK(
        header.total_chunks ==
            header.plane_count * header.chunks_per_plane,
        "total chunk count");
    CHECK(
        header.data_offset ==
            sizeof(AxisPlaneHeader) +
                header.total_chunks * sizeof(AxisPlaneIndexEntry),
        "payload follows fixed index");
    CHECK(
        header.data_offset + header.total_storage_bytes ==
            std::filesystem::file_size(sidecarPath),
        "payload extent matches file size");

    const AxisPlaneShape shape =
        makeAxisPlaneShape(axis, kNx, kNy, kNz);
    std::vector<float> plane(shape.plane_elements);
    CHECK(
        decompressPlane(sidecarPath, header, planeIndex, plane),
        "decompress every chunk in plane");
    verifyPlaneValues(axis, planeIndex, plane);
}

void testProductionReaderMultiChunk() {
    ERWT3DReader reader(lz4Path());
    std::vector<float> yPlane(kNx * kNz);
    std::vector<float> zPlane(kNx * kNy);

    HDDReadWindowConfig windowConfig;
    windowConfig.read_window_bytes = 16ULL << 20;
    windowConfig.max_gap_bytes = 64ULL << 10;

    std::vector<ERWT3DReader::SliceBatchRequest> requests = {
        {SliceAxis::Y, 5, yPlane.data()},
        {SliceAxis::Z, 3, zPlane.data()},
    };
    CHECK(
        reader.readSlicesBatch(requests, 2, 256, windowConfig),
        "production batch reader assembles multi-chunk planes");
    verifyPlaneValues(PlaneAxis::Y, 5, yPlane);
    verifyPlaneValues(PlaneAxis::Z, 3, zPlane);

    std::fill(zPlane.begin(), zPlane.end(), 0.0f);
    CHECK(
        reader.readSlice(SliceAxis::Z, 3, zPlane.data(), 2, 256),
        "production single-slice reader assembles multi-chunk plane");
    verifyPlaneValues(PlaneAxis::Z, 3, zPlane);
}

void testStorageBudgetCleanup() {
    const std::string path =
        axisPlaneSidecarPath(lz4Path(), PlaneAxis::Y);
    std::filesystem::remove(path);

    Lz4AxisPlaneWriterStats stats;
    CHECK(
        !writeLz4AxisPlaneSidecar(
            rawPath(),
            lz4Path(),
            PlaneAxis::Y,
            kNx,
            kNy,
            kNz,
            kChunkElements,
            0.01,
            2,
            &stats),
        "reject sidecar exceeding storage budget");
    CHECK(!stats.written, "rejected sidecar not marked written");
    CHECK(!std::filesystem::exists(path), "rejected sidecar removed");
}

}  // namespace

int main() {
    prepareInput();
    testRoundTrip(PlaneAxis::Y, 5);
    testRoundTrip(PlaneAxis::Z, 3);
    testProductionReaderMultiChunk();
    testStorageBudgetCleanup();
    std::filesystem::remove_all(kTmpDir);

    std::cout << "Passed: " << testsPassed << "/"
              << (testsPassed + testsFailed) << "\n";
    return testsFailed == 0 ? 0 : 1;
}
