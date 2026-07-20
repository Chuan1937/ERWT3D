#include "erwt3d/rzfp_xplane_writer.hpp"
#include "erwt3d/raw_layout.hpp"
#include "erwt3d/thread_pool.hpp"
#include "erwt3d/raw_x_aux.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include "erwt3d/platform_io.hpp"
#include <vector>

namespace erwt3d {

namespace {

constexpr char XPLANE_MAGIC[8] = {'E', 'R', 'W', 'T', '3', 'D', 'X', ' '};
constexpr uint32_t XPLANE_VERSION = 1;
constexpr uint64_t XPLANE_HEADER_SIZE = 256;
constexpr uint64_t XPLANE_INDEX_ENTRY_SIZE = 16;

#pragma pack(push, 1)
struct XPlaneHeader {
    char magic[8];
    uint64_t version;
    uint64_t nx, ny, nz;
    uint64_t data_offset;
    uint64_t reserved[26];
};
#pragma pack(pop)

static_assert(sizeof(XPlaneHeader) == XPLANE_HEADER_SIZE, "XPlaneHeader size mismatch");

#pragma pack(push, 1)
struct XPlaneIndexEntry {
    uint64_t offset;
    uint32_t size;
    uint32_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(XPlaneIndexEntry) == XPLANE_INDEX_ENTRY_SIZE, "XPlaneIndexEntry size mismatch");



} // namespace

bool writeXPlaneSidecarFile(
    const std::string& raw_path,
    const std::string& output_path,
    const RzfpXPlaneCodecConfig& cfg,
    uint64_t nx,
    uint64_t ny,
    uint64_t nz,
    int threads,
    RzfpXPlaneWriterStats* out_stats
) {
    int raw_fd = io_open(raw_path.c_str(), O_RDONLY);
    if (raw_fd < 0) {
        std::cerr << "Error: cannot open raw file for sidecar: " << raw_path << std::endl;
        return false;
    }

    const std::string tmp_path = output_path + ".tmp";
    int out_fd = io_open(tmp_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
        std::cerr << "Error: cannot create sidecar temp file: " << tmp_path << std::endl;
        io_close(raw_fd);
        return false;
    }

    const uint64_t index_offset = XPLANE_HEADER_SIZE;
    const uint64_t data_offset = index_offset + nx * XPLANE_INDEX_ENTRY_SIZE;
    if (posix_fallocate(out_fd, 0, static_cast<int64_t>(data_offset)) != 0) {
        if (ftruncate(out_fd, static_cast<int64_t>(data_offset)) != 0) {
            std::cerr << "Error: failed to reserve sidecar header/index area" << std::endl;
            io_close(raw_fd); io_close(out_fd); unlink(tmp_path.c_str());
            return false;
        }
    }

    XPlaneHeader header{};
    std::memcpy(header.magic, XPLANE_MAGIC, 8);
    header.version = XPLANE_VERSION;
    header.nx = nx;
    header.ny = ny;
    header.nz = nz;
    header.data_offset = data_offset;

    std::vector<XPlaneIndexEntry> index(nx);
    std::atomic<uint64_t> total_compressed{0};

    ThreadPool pool(static_cast<size_t>(std::max(1, threads)));

    const uint64_t chunk_x = std::max<uint64_t>(1, std::min<uint64_t>(nx, 32));
    const uint64_t plane_floats = ny * nz;
    const uint64_t plane_bytes = plane_floats * sizeof(float);

    uint64_t next_data_offset = data_offset;

    for (uint64_t x_start = 0; x_start < nx; x_start += chunk_x) {
        const uint64_t x_end = std::min(x_start + chunk_x, nx);
        const uint64_t current_chunk = x_end - x_start;
        const uint64_t chunk_floats = current_chunk * plane_floats;

        std::vector<float> chunk_buffer(chunk_floats);
        const uint64_t read_offset = x_start * plane_bytes;
        if (!readFullyAt(raw_fd, chunk_buffer.data(), chunk_floats * sizeof(float), read_offset)) {
            std::cerr << "Error: failed to read X-plane chunk x=" << x_start << std::endl;
            io_close(raw_fd); io_close(out_fd); unlink(tmp_path.c_str());
            return false;
        }

        std::vector<std::future<std::vector<uint8_t>>> futures;
        futures.reserve(current_chunk);
        for (uint64_t xi = 0; xi < current_chunk; ++xi) {
            const float* raw_plane = chunk_buffer.data() + xi * plane_floats;
            futures.push_back(pool.submit([raw_plane, ny, nz, &cfg]() {
                // Raw X-plane is Z-fastest (offset = y*nz + z).
                // Sidecar plane format expects Y-fastest (offset = z*ny + y).
                std::vector<float> plane(ny * nz);
                for (uint64_t y = 0; y < ny; ++y) {
                    for (uint64_t z = 0; z < nz; ++z) {
                        plane[z * ny + y] = raw_plane[rawOffsetZFastest(0, y, z, ny, nz)];
                    }
                }
                return encodeXPlane2D(plane.data(), ny, nz, cfg);
            }));
        }

        for (uint64_t xi = 0; xi < current_chunk; ++xi) {
            std::vector<uint8_t> record = futures[xi].get();
            const uint64_t x = x_start + xi;
            const uint32_t size = static_cast<uint32_t>(record.size());
            index[x].offset = next_data_offset;
            index[x].size = size;
            index[x].reserved = 0;

            if (!record.empty()) {
                if (!writeFullyAt(out_fd, record.data(), record.size(), next_data_offset)) {
                    std::cerr << "Error: failed to write sidecar record for x=" << x << std::endl;
                    io_close(raw_fd); io_close(out_fd); unlink(tmp_path.c_str());
                    return false;
                }
            }
            next_data_offset += record.size();
            total_compressed += record.size();
        }

        std::cout << "Sidecar chunk " << (x_start / chunk_x + 1) << "/"
                  << ((nx + chunk_x - 1) / chunk_x) << " done, x=" << x_start << ".." << (x_end - 1)
                  << std::endl;
    }

    pool.waitAll();

    if (!writeFullyAt(out_fd, index.data(), nx * XPLANE_INDEX_ENTRY_SIZE, index_offset)) {
        std::cerr << "Error: failed to write sidecar index" << std::endl;
        io_close(raw_fd); io_close(out_fd); unlink(tmp_path.c_str());
        return false;
    }

    if (!writeFullyAt(out_fd, &header, XPLANE_HEADER_SIZE, 0)) {
        std::cerr << "Error: failed to write sidecar header" << std::endl;
        io_close(raw_fd); io_close(out_fd); unlink(tmp_path.c_str());
        return false;
    }

    io_close(raw_fd);
    io_close(out_fd);

    if (rename(tmp_path.c_str(), output_path.c_str()) != 0) {
        std::cerr << "Error: failed to rename sidecar file" << std::endl;
        unlink(tmp_path.c_str());
        return false;
    }

    if (out_stats) {
        out_stats->plane_count = nx;
        out_stats->total_raw_bytes = nx * ny * nz * sizeof(float);
        out_stats->total_compressed_bytes = total_compressed.load();
        out_stats->compression_ratio = out_stats->total_raw_bytes > 0
            ? static_cast<double>(out_stats->total_compressed_bytes) / static_cast<double>(out_stats->total_raw_bytes)
            : 0.0;
    }

    return true;
}

} // namespace erwt3d
