#include <erwt3d/raw_layout.hpp>
#include <erwt3d/raw_x_aux.hpp>
#include <erwt3d/taps_format.hpp>
#include <erwt3d/thread_pool.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <lz4.h>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace erwt3d {

struct AxisStreamData {
    char axis = 0;
    TapsCodec codec = TapsCodec::Raw;
    uint64_t plane_count = 0;
    uint64_t plane_floats = 0;
    uint64_t chunk_floats = 0;
    std::vector<TapsChunkIndex> chunks;
    int stream_fd = -1;
    uint64_t stream_bytes = 0;
    std::vector<uint64_t> plane_chunk_start;

    bool load(const std::string& dir, char a) {
        axis = a;
        std::string meta_path = dir + "/metadata.bin";
        ScopedFd meta_fd(open(meta_path.c_str(), O_RDONLY));
        if (!meta_fd.valid()) return false;

        uint8_t header[TAPS_HEADER_SIZE];
        if (!readFullyAt(meta_fd.get(), header, TAPS_HEADER_SIZE, 0)) return false;

        uint64_t off = 40;
        for (int i = 0; i < 3; ++i) {
            if (static_cast<char>(header[off]) == a) {
                codec = static_cast<TapsCodec>(header[off + 1]);
                memcpy(&plane_count, header + off + 8, 8);
                memcpy(&plane_floats, header + off + 16, 8);
                memcpy(&chunk_floats, header + off + 24, 8);
                memcpy(&stream_bytes, header + off + 32, 8);
                break;
            }
            off += 64;
        }

        if (plane_count == 0) return false;

        std::string stream_path = dir + "/" + std::string(1, a) + ".stream";
        std::string index_path = dir + "/" + std::string(1, a) + ".index";

        stream_fd = open(stream_path.c_str(), O_RDONLY);
        ScopedFd idx_fd(open(index_path.c_str(), O_RDONLY));
        if (stream_fd < 0 || !idx_fd.valid()) return false;

        struct stat st;
        if (fstat(idx_fd.get(), &st) != 0) return false;
        uint64_t idx_bytes = static_cast<uint64_t>(st.st_size);
        uint64_t idx_entries = idx_bytes / sizeof(TapsChunkIndex);
        if (idx_entries == 0) return false;

        chunks.resize(idx_entries);
        if (!readFullyAt(idx_fd.get(), chunks.data(), idx_bytes, 0)) return false;

        plane_chunk_start.resize(plane_count + 1, UINT64_MAX);
        for (uint64_t i = 0; i < idx_entries; ++i) {
            uint32_t pi = chunks[i].plane_index;
            if (pi < plane_count && plane_chunk_start[pi] == UINT64_MAX) {
                plane_chunk_start[pi] = i;
            }
        }
        plane_chunk_start[plane_count] = idx_entries;

        return true;
    }
};

static void unshufflePlane(float* data, char axis, uint64_t nx, uint64_t ny, uint64_t nz,
                           float* tmp_buf) {
    switch (axis) {
        case 'X': {
            memcpy(tmp_buf, data, ny * nz * sizeof(float));
            for (uint64_t y = 0; y < ny; ++y)
                for (uint64_t z = 0; z < nz; ++z)
                    data[y * nz + z] = tmp_buf[z * ny + y];
            break;
        }
        case 'Y': {
            memcpy(tmp_buf, data, nx * nz * sizeof(float));
            for (uint64_t x = 0; x < nx; ++x)
                for (uint64_t z = 0; z < nz; ++z)
                    data[x * nz + z] = tmp_buf[z * nx + x];
            break;
        }
        case 'Z': {
            memcpy(tmp_buf, data, nx * ny * sizeof(float));
            for (uint64_t x = 0; x < nx; ++x)
                for (uint64_t y = 0; y < ny; ++y)
                    data[x * ny + y] = tmp_buf[y * nx + x];
            break;
        }
    }
}

struct TapsReader::Impl {
    std::string dir;
    AxisStreamData streams[3];
    uint64_t nx = 0, ny = 0, nz = 0;
    bool valid = false;
    int threads = 1;
    std::unique_ptr<ThreadPool> pool;

    Impl(const std::string& d, int t) : dir(d), threads(t) {
        if (threads > 1) {
            pool = std::make_unique<ThreadPool>(threads);
        }
    }

    ~Impl() {
        for (int i = 0; i < 3; ++i) {
            if (streams[i].stream_fd >= 0) close(streams[i].stream_fd);
        }
    }

    bool init() {
        std::string meta_path = dir + "/metadata.bin";
        ScopedFd fd(open(meta_path.c_str(), O_RDONLY));
        if (!fd.valid()) return false;

        uint8_t header[TAPS_HEADER_SIZE];
        if (!readFullyAt(fd.get(), header, TAPS_HEADER_SIZE, 0)) return false;

        uint32_t magic;
        memcpy(&magic, header, 4);
        if (magic != TAPS_MAGIC) return false;

        memcpy(&nx, header + 8, 8);
        memcpy(&ny, header + 16, 8);
        memcpy(&nz, header + 24, 8);

        for (int i = 0; i < 3; ++i) {
            char axis = static_cast<char>(header[40 + i * 64]);
            if (!streams[i].load(dir, axis)) return false;
        }

        valid = true;
        return true;
    }

    int axisIndex(char a) const {
        for (int i = 0; i < 3; ++i)
            if (streams[i].axis == a) return i;
        return -1;
    }

    bool decodePlane(int axis_idx, uint64_t plane_index, float* output) {
        if (plane_index >= streams[axis_idx].plane_count) return false;
        auto& s = streams[axis_idx];

        uint64_t start = s.plane_chunk_start[plane_index];
        uint64_t end = s.plane_chunk_start[plane_index + 1];
        if (start == UINT64_MAX) return false;

        std::vector<uint8_t> comp_buf;
        uint64_t out_offset = 0;

        for (uint64_t ci = start; ci < end; ++ci) {
            auto& chunk = s.chunks[ci];
            uint32_t comp_size = chunk.compressed_size;
            uint32_t raw_size = chunk.raw_size;

            comp_buf.resize(comp_size);
            if (!readFullyAt(s.stream_fd, comp_buf.data(), comp_size, chunk.offset))
                return false;

            if (comp_size < raw_size) {
                int dec = LZ4_decompress_safe(
                    reinterpret_cast<const char*>(comp_buf.data()),
                    reinterpret_cast<char*>(output + out_offset),
                    static_cast<int>(comp_size),
                    static_cast<int>(raw_size)
                );
                if (dec < 0) return false;
            } else {
                memcpy(output + out_offset, comp_buf.data(), raw_size);
            }
            out_offset += raw_size / sizeof(float);
        }

        return true;
    }

    bool readPlane(int axis_idx, uint64_t plane_index, float* output) {
        if (!decodePlane(axis_idx, plane_index, output)) return false;

        uint64_t max_floats = ny * nz;
        if (axis_idx == 1 || axis_idx == 2) max_floats = std::max(nx * nz, nx * ny);

        std::vector<float> tmp(max_floats);
        unshufflePlane(output, streams[axis_idx].axis, nx, ny, nz, tmp.data());
        return true;
    }

    bool readSlicesBatchParallel(const std::vector<TapsSliceRequest>& requests) {
        if (threads <= 1 || !pool) {
            for (auto& req : requests) {
                int ai = axisIndex(req.axis);
                if (ai < 0) return false;
                if (!readPlane(ai, req.index, req.output)) return false;
            }
            return true;
        }

        struct AxisBatch {
            int axis_idx;
            std::vector<size_t> req_indices;
        };

        AxisBatch batches[3];
        for (int i = 0; i < 3; ++i) batches[i].axis_idx = -1;

        for (size_t i = 0; i < requests.size(); ++i) {
            int ai = axisIndex(requests[i].axis);
            if (ai < 0) return false;
            batches[ai].axis_idx = ai;
            batches[ai].req_indices.push_back(i);
        }

        std::atomic<int> failed{0};
        std::vector<std::future<void>> futures;

        for (int b = 0; b < 3; ++b) {
            if (batches[b].axis_idx < 0 || batches[b].req_indices.empty()) continue;

            uint64_t max_floats = ny * nz;
            if (b == 1 || b == 2) max_floats = std::max(nx * nz, nx * ny);
            auto& batch = batches[b];

            for (size_t ri = 0; ri < batch.req_indices.size(); ++ri) {
                size_t req_idx = batch.req_indices[ri];
                float* output = requests[req_idx].output;
                uint64_t plane_index = requests[req_idx].index;

                futures.push_back(pool->submit([this, b, plane_index, output, max_floats, &failed]() {
                    std::vector<float> tmp(max_floats);
                    if (!decodePlane(b, plane_index, output)) {
                        failed.fetch_add(1);
                        return;
                    }
                    unshufflePlane(output, streams[b].axis, nx, ny, nz, tmp.data());
                }));
            }
        }

        for (auto& f : futures) f.get();

        return failed.load() == 0;
    }
};

TapsReader::TapsReader(const std::string& dir, int threads)
    : impl_(std::make_unique<Impl>(dir, threads)) {
    impl_->init();
}

TapsReader::~TapsReader() = default;

bool TapsReader::readSlice(char axis, uint64_t index, float* output) {
    int idx = impl_->axisIndex(axis);
    if (idx < 0) return false;
    return impl_->readPlane(idx, index, output);
}

bool TapsReader::readSlicesBatch(const std::vector<TapsSliceRequest>& requests) {
    return impl_->readSlicesBatchParallel(requests);
}

uint64_t TapsReader::nx() const { return impl_->nx; }
uint64_t TapsReader::ny() const { return impl_->ny; }
uint64_t TapsReader::nz() const { return impl_->nz; }

double TapsReader::storageRatio() const {
    uint64_t raw = impl_->nx * impl_->ny * impl_->nz * sizeof(float);
    uint64_t compressed = 0;
    for (int i = 0; i < 3; ++i)
        compressed += impl_->streams[i].stream_bytes;
    return raw > 0 ? static_cast<double>(compressed) / raw : 0;
}

void TapsReader::setThreads(int t) {
    if (t > 1 && !impl_->pool) {
        impl_->pool = std::make_unique<ThreadPool>(t);
    }
    impl_->threads = t;
}

}
