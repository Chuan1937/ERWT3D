#include "erwt3d/tri_reader.hpp"
#include "erwt3d/tri_format.hpp"

#ifdef ERWT3D_HAVE_ZFP
#include <zfp.h>
#include <zfp/bitstream.h>
#endif

#include <iostream>
#include <cstring>
#include <cmath>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <thread>

namespace erwt3d {

static constexpr uint32_t BLOCK_DIM = 4;
static constexpr uint64_t BLOCK_FLOATS = 64;
static constexpr uint64_t BLOCK_BYTES = 256;

#ifdef ERWT3D_HAVE_ZFP

struct ZfpReadState {
    zfp_stream* zs = nullptr;
    zfp_field* field = nullptr;
    bitstream* bs = nullptr;
    std::vector<uint8_t> compBuf;
    float blockDec[64];

    void init(size_t compBufSize) {
        compBuf.resize(compBufSize, 0);
        std::memset(blockDec, 0, sizeof(blockDec));
        field = zfp_field_3d(blockDec, zfp_type_float, 4, 4, 4);
        zs = zfp_stream_open(nullptr);
        bs = stream_open(compBuf.data(), compBufSize);
        zfp_stream_set_bit_stream(zs, bs);
    }

    ~ZfpReadState() {
        if (field) zfp_field_free(field);
        if (zs) zfp_stream_close(zs);
        if (bs) stream_close(bs);
    }

    bool decompress(const uint8_t* compData, size_t compSize, double rate) {
        zfp_stream_set_rate(zs, rate, zfp_type_float, 3, zfp_false);
        std::memcpy(compBuf.data(), compData, compSize);
        zfp_field_set_pointer(field, blockDec);
        stream_rewind(bs);
        size_t ret = zfp_decompress(zs, field);
        return ret != 0;
    }
};

static thread_local std::unique_ptr<ZfpReadState> tlsZfp;
static size_t g_compBufSize = 0;
static double g_rate = 0;

static ZfpReadState& getZfp() {
    if (!tlsZfp) {
        tlsZfp = std::make_unique<ZfpReadState>();
        tlsZfp->init(g_compBufSize);
    }
    return *tlsZfp;
}

#endif // ERWT3D_HAVE_ZFP

TriReader::TriReader(const std::string& path, size_t cacheMB)
    : path_(path), fd_(-1) {
    fd_ = open(path.c_str(), O_RDONLY);
    if (fd_ < 0) {
        std::cerr << "Error: cannot open tri file " << path << ": " << strerror(errno) << "\n";
        return;
    }

    if (pread(fd_, &header_, sizeof(TriHeader), 0) != sizeof(TriHeader)) {
        std::cerr << "Error: cannot read tri header\n";
        close(fd_); fd_ = -1;
        return;
    }

    if (!validateTriHeader(header_)) {
        std::cerr << "Error: invalid tri header\n";
        close(fd_); fd_ = -1;
        return;
    }

    bxCount_ = triBlockCountX(header_);
    byCount_ = triBlockCountY(header_);
    bzCount_ = triBlockCountZ(header_);

    hasZfp_ = (header_.codec == TRI_CODEC_ZFP_FIXED_RATE);
    rate_ = (double)header_.rate_bpv;
    blockBytes_ = header_.axis_block_bytes[0];

    if (hasZfp_) {
        g_compBufSize = blockBytes_ + 64;
        g_rate = rate_;
    }

    hasException_ = (header_.flags & TRI_FLAG_HAS_EXCEPTION) != 0;
    if (hasException_) {
        loadExceptionIndex();
    }

    if (hddMode_) {
        posix_fadvise(fd_, 0, 0, POSIX_FADV_RANDOM);
    }
}

TriReader::~TriReader() {
    if (fd_ >= 0) close(fd_);
}

uint64_t TriReader::axisBlockOffset(TriSliceAxis axis, uint64_t outerIdx) const {
    uint64_t blocksPerOuter;
    if (axis == TriSliceAxis::X) {
        blocksPerOuter = bzCount_ * byCount_;
    } else if (axis == TriSliceAxis::Y) {
        blocksPerOuter = bzCount_ * bxCount_;
    } else {
        blocksPerOuter = byCount_ * bxCount_;
    }
    return header_.axis_offsets[(int)axis] + outerIdx * blocksPerOuter * blockBytes_;
}

uint64_t TriReader::axisBlockCount(TriSliceAxis axis) const {
    if (axis == TriSliceAxis::X) return bzCount_ * byCount_;
    if (axis == TriSliceAxis::Y) return bzCount_ * bxCount_;
    return byCount_ * bxCount_;
}

void TriReader::loadExceptionIndex() {
    if (header_.exception_count == 0) return;
    excIndex_.resize(header_.exception_count);
    pread(fd_, excIndex_.data(),
          header_.exception_count * sizeof(TriExceptionIndex),
          header_.exception_index_offset);
}

bool TriReader::isExceptionBlock(uint64_t bx, uint64_t by, uint64_t bz) const {
    if (!hasException_) return false;
    uint64_t gid = (bz * byCount_ + by) * bxCount_ + bx;
    auto it = std::lower_bound(excIndex_.begin(), excIndex_.end(), gid,
        [](const TriExceptionIndex& e, uint64_t v) { return e.block_id < v; });
    return it != excIndex_.end() && it->block_id == gid;
}

bool TriReader::readExceptionBlock(uint64_t bx, uint64_t by, uint64_t bz, float* blockOut) {
    uint64_t gid = (bz * byCount_ + by) * bxCount_ + bx;
    auto it = std::lower_bound(excIndex_.begin(), excIndex_.end(), gid,
        [](const TriExceptionIndex& e, uint64_t v) { return e.block_id < v; });
    if (it == excIndex_.end() || it->block_id != gid) return false;

    pread(fd_, blockOut, BLOCK_BYTES, it->data_offset);
    return true;
}

bool TriReader::readSlice(TriSliceAxis axis, uint64_t index, float* output) {
    TriProfile profile;
    if (hasZfp_) return readSliceZfp(axis, index, output, &profile);
    return readSliceRaw(axis, index, output, &profile);
}

void TriReader::setHDDMode() {
    hddMode_ = true;
    if (fd_ >= 0) posix_fadvise(fd_, 0, 0, POSIX_FADV_SEQUENTIAL);
}

void TriReader::prefetchSlab(TriSliceAxis axis, uint64_t index) {
    if (fd_ < 0) return;
    uint64_t off = slabOffsetFor(axis, index);
    uint64_t bytes = slabBytesFor(axis);
    readahead(fd_, off, bytes);
}

bool TriReader::readSliceZfp(TriSliceAxis axis, uint64_t index, float* output,
                              TriProfile* profile) {
    auto tStart = std::chrono::high_resolution_clock::now();

    uint64_t nx = header_.nx, ny = header_.ny, nz = header_.nz;

    uint64_t outerIdx, intraIdx;
    uint64_t outerMax;
    uint64_t outDim1, outDim2;
    uint64_t inner1Max, inner2Max;

    if (axis == TriSliceAxis::X) {
        outerIdx = index / BLOCK_DIM;
        intraIdx = index % BLOCK_DIM;
        outerMax = bxCount_;
        inner1Max = byCount_;
        inner2Max = bzCount_;
        outDim1 = ny; outDim2 = nz;
    } else if (axis == TriSliceAxis::Y) {
        outerIdx = index / BLOCK_DIM;
        intraIdx = index % BLOCK_DIM;
        outerMax = byCount_;
        inner1Max = bxCount_;
        inner2Max = bzCount_;
        outDim1 = nx; outDim2 = nz;
    } else {
        outerIdx = index / BLOCK_DIM;
        intraIdx = index % BLOCK_DIM;
        outerMax = bzCount_;
        inner1Max = bxCount_;
        inner2Max = byCount_;
        outDim1 = nx; outDim2 = ny;
    }

    if (outerIdx >= outerMax) {
        std::cerr << "Error: slice index " << index << " out of range\n";
        return false;
    }

    uint64_t blocksPerOuter = axisBlockCount(axis);
    uint64_t slabOffset = axisBlockOffset(axis, outerIdx);
    uint64_t slabBytes = blocksPerOuter * blockBytes_;

    if (slabBuf_.size() < slabBytes) slabBuf_.resize(slabBytes);

    auto tReadStart = std::chrono::high_resolution_clock::now();
    uint64_t remaining = slabBytes;
    uint64_t off = slabOffset;
    char* p = reinterpret_cast<char*>(slabBuf_.data());
    while (remaining > 0) {
        ssize_t rd = pread(fd_, p, remaining, off);
        if (rd <= 0) {
            std::cerr << "Error reading slab at offset " << off << "\n";
            return false;
        }
        remaining -= rd; off += rd; p += rd;
    }
    auto tReadEnd = std::chrono::high_resolution_clock::now();
    profile->read_time_ms = std::chrono::duration<double, std::milli>(tReadEnd - tReadStart).count();
    profile->pread_calls = 1;
    profile->bytes_read = slabBytes;

    auto tDecodeStart = std::chrono::high_resolution_clock::now();
    std::memset(output, 0, outDim1 * outDim2 * sizeof(float));

    int nThreads = numThreads_;
    if (nThreads > 1 && !pool_) pool_ = std::make_unique<ThreadPool>(nThreads);
    if (nThreads <= 1 || blocksPerOuter < 64) nThreads = 1;

    uint64_t excCount = 0, excBytes = 0;

    auto decodeRange = [&](uint64_t biStart, uint64_t biEnd) {
        ZfpReadState& zfp = getZfp();
        float rawBlock[64];

        for (uint64_t bi = biStart; bi < biEnd; ++bi) {
            uint64_t i1, i2;
            if (axis == TriSliceAxis::X) {
                i2 = bi / byCount_;
                i1 = bi % byCount_;
            } else if (axis == TriSliceAxis::Y) {
                i2 = bi / bxCount_;
                i1 = bi % bxCount_;
            } else {
                i2 = bi / bxCount_;
                i1 = bi % bxCount_;
            }

            const uint8_t* compData = slabBuf_.data() + bi * blockBytes_;
            zfp.decompress(compData, blockBytes_, g_rate);

            uint64_t bx, by, bz;
            if (axis == TriSliceAxis::X) { bx = outerIdx; by = i1; bz = i2; }
            else if (axis == TriSliceAxis::Y) { by = outerIdx; bx = i1; bz = i2; }
            else { bz = outerIdx; bx = i1; by = i2; }

            if (isExceptionBlock(bx, by, bz)) {
                if (readExceptionBlock(bx, by, bz, rawBlock)) {
                    std::memcpy(zfp.blockDec, rawBlock, BLOCK_BYTES);
                    excCount++;
                    excBytes += BLOCK_BYTES;
                }
            }

            for (uint32_t d2 = 0; d2 < BLOCK_DIM; ++d2) {
                for (uint32_t d1 = 0; d1 < BLOCK_DIM; ++d1) {
                    uint64_t outD1 = i1 * BLOCK_DIM + d1;
                    uint64_t outD2 = i2 * BLOCK_DIM + d2;
                    if (outD1 >= outDim1 || outD2 >= outDim2) continue;

                    uint64_t blockIdx;
                    if (axis == TriSliceAxis::X) {
                        blockIdx = (d2 * BLOCK_DIM + d1) * BLOCK_DIM + intraIdx;
                    } else if (axis == TriSliceAxis::Y) {
                        blockIdx = (d2 * BLOCK_DIM + intraIdx) * BLOCK_DIM + d1;
                    } else {
                        blockIdx = (intraIdx * BLOCK_DIM + d2) * BLOCK_DIM + d1;
                    }

                    output[outD2 * outDim1 + outD1] = zfp.blockDec[blockIdx];
                }
            }
        }
    };

    if (nThreads <= 1) {
        decodeRange(0, blocksPerOuter);
    } else {
        uint64_t chunkSize = (blocksPerOuter + nThreads - 1) / nThreads;
        std::vector<std::future<void>> futures;
        for (int t = 0; t < nThreads; ++t) {
            uint64_t s = (uint64_t)t * chunkSize;
            uint64_t e = std::min(s + chunkSize, blocksPerOuter);
            if (s >= e) break;
            futures.push_back(pool_->submit([&, s, e]() { decodeRange(s, e); }));
        }
        for (auto& f : futures) f.get();
    }

    profile->blocks_decoded = blocksPerOuter;
    profile->exception_blocks = excCount;
    profile->exception_bytes_read = excBytes;

    auto tDecodeEnd = std::chrono::high_resolution_clock::now();
    profile->decode_time_ms = std::chrono::duration<double, std::milli>(tDecodeEnd - tDecodeStart).count();

    auto tEnd = std::chrono::high_resolution_clock::now();
    profile->total_ms = std::chrono::duration<double, std::milli>(tEnd - tStart).count();

    if (profileIO_) lastProfile_ = *profile;
    return true;
}

bool TriReader::readSliceRaw(TriSliceAxis axis, uint64_t index, float* output,
                              TriProfile* profile) {
    std::cerr << "Error: raw tri codec not implemented\n";
    return false;
}

bool TriReader::readSlicesBatch(const std::vector<TriSliceBatchRequest>& reqs,
                                 int numThreads, size_t memoryLimitMB,
                                 const HDDReadWindowConfig& wcfg) {
    for (const auto& req : reqs) {
        if (!readSlice(req.axis, req.index, req.output)) return false;
    }
    return true;
}

} // namespace erwt3d
