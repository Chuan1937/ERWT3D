#include <erwt3d/raw_layout.hpp>
#include <erwt3d/raw_x_aux.hpp>
#include <erwt3d/taps_format.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <lz4.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace erwt3d {

struct AxisWriteState {
    char axis;
    uint64_t plane_count;
    uint64_t plane_floats;
    uint64_t chunk_floats;
    TapsCodec codec;
    std::string stream_path;
    std::string index_path;
    std::vector<TapsChunkIndex> chunks;
    uint64_t stream_bytes = 0;
};

static uint64_t compressAndWriteChunks(
    const float* data, uint64_t floats, uint64_t chunk_floats,
    int fd, uint64_t base_offset, std::vector<TapsChunkIndex>& chunks,
    uint32_t plane_index, TapsCodec codec
) {
    std::vector<uint8_t> tmp(LZ4_compressBound(static_cast<int>(chunk_floats * sizeof(float))));
    uint64_t total_compressed = 0;
    uint64_t offset = base_offset;

    for (uint64_t off = 0; off < floats; off += chunk_floats) {
        uint64_t n = std::min(chunk_floats, floats - off);
        int srcSize = static_cast<int>(n * sizeof(float));
        int dstCap = LZ4_compressBound(srcSize);
        int compressed = LZ4_compress_default(
            reinterpret_cast<const char*>(data + off),
            reinterpret_cast<char*>(tmp.data()),
            srcSize, dstCap
        );

        uint32_t comp_size;
        if (compressed > 0 && static_cast<uint32_t>(compressed) < static_cast<uint32_t>(srcSize)) {
            comp_size = static_cast<uint32_t>(compressed);
            writeFullyAt(fd, tmp.data(), comp_size, offset);
        } else {
            comp_size = static_cast<uint32_t>(srcSize);
            writeFullyAt(fd, data + off, comp_size, offset);
        }

        TapsChunkIndex ci{};
        ci.offset = offset;
        ci.compressed_size = comp_size;
        ci.raw_size = static_cast<uint32_t>(srcSize);
        ci.plane_index = plane_index;
        ci.codec = static_cast<uint8_t>(codec);
        chunks.push_back(ci);

        offset += comp_size;
        total_compressed += comp_size;
    }
    return total_compressed;
}

static bool writeAxisStreamX(int raw_fd, AxisWriteState& state,
                              uint64_t nx, uint64_t ny, uint64_t nz) {
    ScopedFd stream_fd(open(state.stream_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644));
    if (!stream_fd.valid()) return false;

    uint64_t current_offset = 0;
    uint64_t plane_bytes = rawXPlaneBytes(ny, nz);
    std::vector<float> raw_plane(state.plane_floats);
    std::vector<float> rearranged(state.plane_floats);

    for (uint64_t i = 0; i < state.plane_count; ++i) {
        uint64_t off = rawXPlaneBytes(ny, nz) * i;
        if (!readFullyAt(raw_fd, raw_plane.data(), plane_bytes, off)) return false;

        constexpr uint64_t BY = 64, BZ = 64;
        for (uint64_t y0 = 0; y0 < ny; y0 += BY) {
            uint64_t ye = std::min(y0 + BY, ny);
            for (uint64_t z0 = 0; z0 < nz; z0 += BZ) {
                uint64_t ze = std::min(z0 + BZ, nz);
                for (uint64_t y = y0; y < ye; ++y)
                    for (uint64_t z = z0; z < ze; ++z)
                        rearranged[z * ny + y] = raw_plane[y * nz + z];
            }
        }

        uint64_t written = compressAndWriteChunks(
            rearranged.data(), state.plane_floats, state.chunk_floats,
            stream_fd.get(), current_offset, state.chunks,
            static_cast<uint32_t>(i), state.codec
        );
        current_offset += written;

        if ((i % 50 == 0) || (i + 1 == state.plane_count)) {
            std::fprintf(stderr, "\r  %c-stream: %lu/%lu planes, %.1f MiB   ",
                         state.axis, i + 1, state.plane_count,
                         current_offset / (1024.0 * 1024.0));
            std::fflush(stderr);
        }
    }
    std::fprintf(stderr, "\n");
    state.stream_bytes = current_offset;
    return true;
}

static bool writeAxisStreamY(int raw_fd, AxisWriteState& state,
                              uint64_t nx, uint64_t ny, uint64_t nz) {
    ScopedFd stream_fd(open(state.stream_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644));
    if (!stream_fd.valid()) return false;

    uint64_t current_offset = 0;
    std::vector<float> raw_plane(state.plane_floats);
    std::vector<float> rearranged(state.plane_floats);

    for (uint64_t i = 0; i < state.plane_count; ++i) {
        for (uint64_t x = 0; x < nx; ++x) {
            uint64_t off = rawOffsetBytesZFastest(x, i, 0, ny, nz);
            if (!readFullyAt(raw_fd, raw_plane.data() + x * nz, nz * sizeof(float), off))
                return false;
        }

        for (uint64_t x = 0; x < nx; ++x)
            for (uint64_t z = 0; z < nz; ++z)
                rearranged[z * nx + x] = raw_plane[x * nz + z];

        uint64_t written = compressAndWriteChunks(
            rearranged.data(), state.plane_floats, state.chunk_floats,
            stream_fd.get(), current_offset, state.chunks,
            static_cast<uint32_t>(i), state.codec
        );
        current_offset += written;

        if ((i % 200 == 0) || (i + 1 == state.plane_count)) {
            std::fprintf(stderr, "\r  %c-stream: %lu/%lu planes, %.1f MiB   ",
                         state.axis, i + 1, state.plane_count,
                         current_offset / (1024.0 * 1024.0));
            std::fflush(stderr);
        }
    }
    std::fprintf(stderr, "\n");
    state.stream_bytes = current_offset;
    return true;
}

static bool writeAxisStreamZ(int raw_fd, AxisWriteState& state,
                              uint64_t nx, uint64_t ny, uint64_t nz) {
    ScopedFd stream_fd(open(state.stream_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644));
    if (!stream_fd.valid()) return false;

    uint64_t current_offset = 0;
    uint64_t yz_floats = ny * nz;
    std::vector<float> xplane(yz_floats);
    std::vector<float> rearranged(state.plane_floats);
    std::vector<float> z_buf(state.plane_count * state.plane_floats, 0.0f);

    for (uint64_t x = 0; x < nx; ++x) {
        uint64_t off = rawXPlaneOffset(x, ny, nz) * sizeof(float);
        if (!readFullyAt(raw_fd, xplane.data(), yz_floats * sizeof(float), off))
            return false;

        for (uint64_t y = 0; y < ny; ++y) {
            for (uint64_t z = 0; z < nz; ++z) {
                z_buf[z * (nx * ny) + x * ny + y] = xplane[y * nz + z];
            }
        }

        if ((x % 100 == 0) || (x + 1 == nx)) {
            std::fprintf(stderr, "\r  %c-stream: reading X-slab %lu/%lu   ",
                         state.axis, x + 1, nx);
            std::fflush(stderr);
        }
    }
    std::fprintf(stderr, "\n");

    for (uint64_t z = 0; z < state.plane_count; ++z) {
        float* raw = z_buf.data() + z * nx * ny;

        for (uint64_t x = 0; x < nx; ++x)
            for (uint64_t y = 0; y < ny; ++y)
                rearranged[y * nx + x] = raw[x * ny + y];

        uint64_t written = compressAndWriteChunks(
            rearranged.data(), state.plane_floats, state.chunk_floats,
            stream_fd.get(), current_offset, state.chunks,
            static_cast<uint32_t>(z), state.codec
        );
        current_offset += written;

        if ((z % 200 == 0) || (z + 1 == state.plane_count)) {
            std::fprintf(stderr, "\r  %c-stream: compressing %lu/%lu planes, %.1f MiB   ",
                         state.axis, z + 1, state.plane_count,
                         current_offset / (1024.0 * 1024.0));
            std::fflush(stderr);
        }
    }
    std::fprintf(stderr, "\n");
    state.stream_bytes = current_offset;
    return true;
}

bool tapsWriteFromRaw(const std::string& raw_path, const TapsWriteConfig& config, TapsStats& stats) {
    auto t0 = std::chrono::high_resolution_clock::now();

    ScopedFd raw_fd(open(raw_path.c_str(), O_RDONLY));
    if (!raw_fd.valid()) {
        std::fprintf(stderr, "Cannot open raw file: %s\n", raw_path.c_str());
        return false;
    }

    mkdir(config.output_dir.c_str(), 0755);

    uint64_t chunk_floats = (config.chunk_kb * 1024) / sizeof(float);

    AxisWriteState axes[3];
    axes[0].axis = 'X'; axes[0].plane_count = config.nx; axes[0].plane_floats = config.ny * config.nz;
    axes[1].axis = 'Y'; axes[1].plane_count = config.ny; axes[1].plane_floats = config.nx * config.nz;
    axes[2].axis = 'Z'; axes[2].plane_count = config.nz; axes[2].plane_floats = config.nx * config.ny;

    for (int i = 0; i < 3; ++i) {
        axes[i].codec = config.codec;
        axes[i].chunk_floats = std::min(chunk_floats, axes[i].plane_floats);
        axes[i].stream_path = config.output_dir + "/" + axes[i].axis + ".stream";
        axes[i].index_path = config.output_dir + "/" + axes[i].axis + ".index";
    }

    uint64_t total_raw = config.nx * config.ny * config.nz * sizeof(float);
    stats.total_raw_bytes = total_raw;

    std::fprintf(stderr, "Writing X-stream (%lu planes, %.1f MiB/plane)...\n",
                 axes[0].plane_count, axes[0].plane_floats * sizeof(float) / (1024.0 * 1024.0));
    if (!writeAxisStreamX(raw_fd.get(), axes[0], config.nx, config.ny, config.nz))
        return false;
    stats.total_compressed_bytes += axes[0].stream_bytes;
    std::fprintf(stderr, "  X-stream done: %.2f MiB, ratio=%.4fx\n",
                 axes[0].stream_bytes / (1024.0 * 1024.0),
                 static_cast<double>(axes[0].stream_bytes) /
                 (axes[0].plane_count * axes[0].plane_floats * sizeof(float)));

    std::fprintf(stderr, "Writing Y-stream (%lu planes, %.1f MiB/plane)...\n",
                 axes[1].plane_count, axes[1].plane_floats * sizeof(float) / (1024.0 * 1024.0));
    if (!writeAxisStreamY(raw_fd.get(), axes[1], config.nx, config.ny, config.nz))
        return false;
    stats.total_compressed_bytes += axes[1].stream_bytes;
    std::fprintf(stderr, "  Y-stream done: %.2f MiB, ratio=%.4fx\n",
                 axes[1].stream_bytes / (1024.0 * 1024.0),
                 static_cast<double>(axes[1].stream_bytes) /
                 (axes[1].plane_count * axes[1].plane_floats * sizeof(float)));

    std::fprintf(stderr, "Writing Z-stream (%lu planes, %.1f MiB/plane)...\n",
                 axes[2].plane_count, axes[2].plane_floats * sizeof(float) / (1024.0 * 1024.0));
    if (!writeAxisStreamZ(raw_fd.get(), axes[2], config.nx, config.ny, config.nz))
        return false;
    stats.total_compressed_bytes += axes[2].stream_bytes;
    std::fprintf(stderr, "  Z-stream done: %.2f MiB, ratio=%.4fx\n",
                 axes[2].stream_bytes / (1024.0 * 1024.0),
                 static_cast<double>(axes[2].stream_bytes) /
                 (axes[2].plane_count * axes[2].plane_floats * sizeof(float)));

    for (int i = 0; i < 3; ++i) {
        ScopedFd idx_fd(open(axes[i].index_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644));
        if (!idx_fd.valid()) return false;
        if (!axes[i].chunks.empty()) {
            writeFullyAt(idx_fd.get(), axes[i].chunks.data(),
                         axes[i].chunks.size() * sizeof(TapsChunkIndex), 0);
        }
    }

    std::string meta_path = config.output_dir + "/metadata.bin";
    ScopedFd meta_fd(open(meta_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644));
    if (!meta_fd.valid()) return false;

    uint8_t header[TAPS_HEADER_SIZE];
    memset(header, 0, TAPS_HEADER_SIZE);

    uint32_t magic = TAPS_MAGIC;
    uint32_t version = TAPS_VERSION;
    memcpy(header + 0, &magic, 4);
    memcpy(header + 4, &version, 4);
    memcpy(header + 8, &config.nx, 8);
    memcpy(header + 16, &config.ny, 8);
    memcpy(header + 24, &config.nz, 8);
    uint32_t axis_count = 3;
    memcpy(header + 32, &axis_count, 4);

    uint64_t off = 40;
    for (int i = 0; i < 3; ++i) {
        header[off] = static_cast<uint8_t>(axes[i].axis);
        header[off + 1] = static_cast<uint8_t>(axes[i].codec);
        memcpy(header + off + 8, &axes[i].plane_count, 8);
        memcpy(header + off + 16, &axes[i].plane_floats, 8);
        memcpy(header + off + 24, &axes[i].chunk_floats, 8);
        memcpy(header + off + 32, &axes[i].stream_bytes, 8);
        uint64_t idx_entries = axes[i].chunks.size();
        memcpy(header + off + 40, &idx_entries, 8);
        off += 64;
    }

    writeFullyAt(meta_fd.get(), header, TAPS_HEADER_SIZE, 0);

    auto t1 = std::chrono::high_resolution_clock::now();
    stats.write_seconds = std::chrono::duration<double>(t1 - t0).count();
    stats.storage_ratio = static_cast<double>(stats.total_compressed_bytes) / total_raw;

    std::fprintf(stderr, "TAPS write complete: %.2f GiB total, ratio=%.4fx, %.1f seconds\n",
                 stats.total_compressed_bytes / (1024.0 * 1024.0 * 1024.0),
                 stats.storage_ratio, stats.write_seconds);

    return true;
}

}
