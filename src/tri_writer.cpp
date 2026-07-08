#include "erwt3d/tri_writer.hpp"
#include "erwt3d/thread_pool.hpp"
#include "erwt3d/tri_format.hpp"

#ifdef ERWT3D_HAVE_ZFP
#include <zfp.h>
#include <zfp/bitstream.h>
#endif

#include <iostream>
#include <fstream>
#include <vector>
#include <array>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <thread>
#include <future>
#include <chrono>
#include <iomanip>

namespace erwt3d {

static constexpr uint32_t BLOCK_DIM = 4;
static constexpr uint64_t BLOCK_FLOATS = 64;
static constexpr uint64_t BLOCK_BYTES = 256;

static inline bool isFailedCheck(float ref, float actual, double relTol, double zeroAbsTol) {
    double absErr = std::abs((double)ref - (double)actual);
    double absRef = std::abs((double)ref);
    if (absRef <= zeroAbsTol) return absErr > zeroAbsTol;
    double relErr = absErr / std::max(absRef, 1e-12);
    return relErr >= relTol;
}

#ifdef ERWT3D_HAVE_ZFP

struct ZfpWriteState {
    zfp_stream* zs = nullptr;
    zfp_field* field = nullptr;
    bitstream* bs = nullptr;
    std::vector<uint8_t> compBuf;

    void init(size_t compBufSize) {
        compBuf.resize(compBufSize, 0);
        float dummy[64] = {};
        field = zfp_field_3d(dummy, zfp_type_float, 4, 4, 4);
        zs = zfp_stream_open(nullptr);
        bs = stream_open(compBuf.data(), compBufSize);
        zfp_stream_set_bit_stream(zs, bs);
    }

    ~ZfpWriteState() {
        if (field) zfp_field_free(field);
        if (zs) zfp_stream_close(zs);
        if (bs) stream_close(bs);
    }

    size_t compress(const float* block, double rate, float* decBlock) {
        zfp_stream_set_rate(zs, rate, zfp_type_float, 3, zfp_false);
        zfp_field_set_pointer(field, const_cast<float*>(block));
        stream_rewind(bs);
        size_t compBytes = zfp_compress(zs, field);

        // Decompress for verification
        zfp_field_set_pointer(field, decBlock);
        stream_rewind(bs);
        zfp_decompress(zs, field);

        return compBytes;
    }
};

static thread_local std::unique_ptr<ZfpWriteState> tlsZfp;
static size_t g_compBufSize = 0;
static double g_rate = 0;

static ZfpWriteState& getZfp() {
    if (!tlsZfp) {
        tlsZfp = std::make_unique<ZfpWriteState>();
        tlsZfp->init(g_compBufSize);
    }
    return *tlsZfp;
}

struct TaskOutput {
    std::vector<uint8_t> compData;
    std::vector<uint8_t> rawBlock; // non-empty only if exception
    uint64_t fileOffset = 0;       // compressed block file offset
    uint64_t excDataOff = 0;       // exception raw data file offset (0 if not exception)
    bool isException = false;
};

// Write a sorted vector of (offset, data) pairs to fd, merging contiguous ranges.
// Inputs are sorted by offset ascending.
static void writeSortedEntries(int fd,
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>>& entries) {
    if (entries.empty()) return;

    // Merge contiguous entries into larger writes
    size_t i = 0;
    while (i < entries.size()) {
        uint64_t startOff = entries[i].first;
        size_t endIdx = i;
        size_t totalBytes = entries[i].second.size();

        while (endIdx + 1 < entries.size() &&
               entries[endIdx + 1].first == entries[endIdx].first + entries[endIdx].second.size()) {
            endIdx++;
            totalBytes += entries[endIdx].second.size();
        }

        if (endIdx == i) {
            // Single block
            pwrite(fd, entries[i].second.data(), entries[i].second.size(), startOff);
        } else {
            // Contiguous range - merge into one write
            std::vector<uint8_t> buf(totalBytes);
            size_t pos = 0;
            for (size_t j = i; j <= endIdx; ++j) {
                std::memcpy(buf.data() + pos, entries[j].second.data(), entries[j].second.size());
                pos += entries[j].second.size();
            }
            pwrite(fd, buf.data(), buf.size(), startOff);
        }
        i = endIdx + 1;
    }
}

bool writeTriAxisLayout(const std::string& rawPath,
                        const std::string& outPath,
                        uint64_t nx, uint64_t ny, uint64_t nz,
                        uint32_t codec, uint32_t rate_bpv,
                        double relTol, double zeroAbsTol,
                        int numThreads, size_t memoryLimitMB) {
    if (codec != TRI_CODEC_ZFP_FIXED_RATE) {
        std::cerr << "Error: only ZFP fixed-rate codec supported for now\n";
        return false;
    }

    int fdRaw = open(rawPath.c_str(), O_RDONLY);
    if (fdRaw < 0) {
        std::cerr << "Error: cannot open " << rawPath << ": " << strerror(errno) << "\n";
        return false;
    }
    posix_fadvise(fdRaw, 0, 0, POSIX_FADV_SEQUENTIAL);

    uint64_t bxCount = (nx + BLOCK_DIM - 1) / BLOCK_DIM;
    uint64_t byCount = (ny + BLOCK_DIM - 1) / BLOCK_DIM;
    uint64_t bzCount = (nz + BLOCK_DIM - 1) / BLOCK_DIM;
    uint64_t totalBlocks = bxCount * byCount * bzCount;
    uint64_t rowFloats = nx * ny;

    double rate = (double)rate_bpv;
    uint64_t blockCompBytes = triZfpBlockBytes(rate);
    g_compBufSize = blockCompBytes + 64;
    g_rate = rate;

    uint64_t blocksPerOuter[3] = {
        bzCount * byCount,  // X
        bzCount * bxCount,  // Y
        byCount * bxCount   // Z
    };

    uint64_t axisBlockCounts[3] = {
        blocksPerOuter[0] * bxCount,
        blocksPerOuter[1] * byCount,
        blocksPerOuter[2] * bzCount
    };

    uint64_t axisBytes[3] = {
        axisBlockCounts[0] * blockCompBytes,
        axisBlockCounts[1] * blockCompBytes,
        axisBlockCounts[2] * blockCompBytes
    };

    int fdOut = open(outPath.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fdOut < 0) {
        std::cerr << "Error: cannot create " << outPath << ": " << strerror(errno) << "\n";
        close(fdRaw);
        return false;
    }

    TriHeader header;
    initTriHeader(header);
    header.nx = nx; header.ny = ny; header.nz = nz;
    header.codec = codec;
    header.rate_bpv = rate_bpv;
    header.flags = 0;
    header.data_offset = sizeof(TriHeader);

    uint64_t axisOffsets[3];
    axisOffsets[0] = sizeof(TriHeader);
    axisOffsets[1] = axisOffsets[0] + axisBytes[0];
    axisOffsets[2] = axisOffsets[1] + axisBytes[1];
    uint64_t exceptionIndexStart = axisOffsets[2] + axisBytes[2];
    uint64_t exceptionDataStart = exceptionIndexStart + totalBlocks * sizeof(TriExceptionIndex);

    uint64_t totalFileSize = exceptionDataStart;
    if (ftruncate(fdOut, totalFileSize) != 0) {
        std::cerr << "Error: ftruncate failed\n";
        close(fdOut); close(fdRaw);
        return false;
    }

    std::atomic<uint64_t> exceptionCount{0};
    std::atomic<uint64_t> exceptionDataOff;
    exceptionDataOff.store(exceptionDataStart);
    std::vector<TriExceptionIndex> excPairs;

    std::cerr << "Tri-axis ZFP writer (single-pass)\n";
    std::cerr << "  raw: " << rawPath << " (" << nx << "x" << ny << "x" << nz << ")\n";
    std::cerr << "  blocks: " << bxCount << "x" << byCount << "x" << bzCount
              << " = " << totalBlocks << "\n";
    std::cerr << "  rate: " << rate_bpv << " bpp, block_comp_bytes: " << blockCompBytes << "\n";
    std::cerr << "  axis bytes: X=" << (axisBytes[0]/1e9) << "GB Y=" << (axisBytes[1]/1e9)
              << "GB Z=" << (axisBytes[2]/1e9) << "GB\n";
    double totalAxisGB = (axisBytes[0] + axisBytes[1] + axisBytes[2]) / 1e9;
    double rawGB = (double)(nx * ny * nz * 4) / 1e9;
    std::cerr << "  total axis: " << totalAxisGB << "GB, ratio: " << (totalAxisGB / rawGB) << "x\n";
    std::cerr << "  threads: " << numThreads << "\n";

    size_t memLimitBytes = memoryLimitMB * 1024ULL * 1024ULL;
    size_t perLayerBytes = (size_t)rowFloats * sizeof(float);
    int slabZ = 64;
    if (perLayerBytes > 0 && memLimitBytes > 0) {
        int maxSlabZ = (int)(memLimitBytes / perLayerBytes);
        if (maxSlabZ < (int)BLOCK_DIM) maxSlabZ = BLOCK_DIM;
        // Use up to 256 z-layers per slab if memory allows, to amortize I/O.
        int targetSlabZ = (int)(maxSlabZ * 2 / 3);
        if (targetSlabZ > 256) targetSlabZ = 256;
        if (targetSlabZ > slabZ) slabZ = targetSlabZ;
        if (slabZ > maxSlabZ) slabZ = maxSlabZ;
    }
    if (slabZ > (int)nz) slabZ = nz;
    if (slabZ % BLOCK_DIM != 0) slabZ = (slabZ / BLOCK_DIM) * BLOCK_DIM;
    if (slabZ < (int)BLOCK_DIM) slabZ = BLOCK_DIM;
    std::cerr << "  slab_z: " << slabZ << " (" << ((size_t)rowFloats * slabZ * 4) / (1024*1024) << " MB/slab)\n";

    ThreadPool pool(numThreads);

    std::atomic<uint64_t> blocksCompressed{0};
    std::atomic<bool> doneFlag{false};
    std::mutex progressMutex;

    auto tStart = std::chrono::high_resolution_clock::now();

    std::thread progressThread([&]() {
        auto ptStart = std::chrono::high_resolution_clock::now();
        while (!doneFlag.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            auto now = std::chrono::high_resolution_clock::now();
            double elapsed = std::chrono::duration<double>(now - ptStart).count();
            uint64_t doneBlocks = blocksCompressed.load();
            double pct = 100.0 * doneBlocks / totalBlocks;
            double rateBps = doneBlocks / std::max(elapsed, 1e-6);
            double remaining = (totalBlocks - doneBlocks) / std::max(rateBps, 1.0);
            std::lock_guard<std::mutex> lock(progressMutex);
            std::cerr << "\r  compress: " << doneBlocks << "/" << totalBlocks
                      << " (" << std::fixed << std::setprecision(1) << pct << "%)"
                      << " elapsed " << std::setprecision(0) << elapsed << "s"
                      << " eta " << std::setprecision(0) << remaining << "s"
                      << " rate " << std::setprecision(0) << rateBps << " blk/s     "
                      << std::flush;
        }
    });

    uint64_t totalSlabs = (nz + slabZ - 1) / slabZ;
    std::vector<float> slab((size_t)rowFloats * slabZ, 0.0f);

    for (uint64_t slabIdx = 0, zSlabStart = 0; zSlabStart < nz; ++slabIdx, zSlabStart += slabZ) {
        uint64_t zSlabEnd = std::min(zSlabStart + (uint64_t)slabZ, nz);

        // Read raw z-slab once (sequential)
        for (uint64_t z = zSlabStart; z < zSlabEnd; ++z) {
            uint64_t rowOff = z * rowFloats * sizeof(float);
            char* dst = reinterpret_cast<char*>(slab.data() + (z - zSlabStart) * rowFloats);
            uint64_t remaining = rowFloats * sizeof(float);
            uint64_t off = rowOff;
            while (remaining > 0) {
                ssize_t rd = pread(fdRaw, dst, remaining, off);
                if (rd <= 0) {
                    std::cerr << "\nError reading z-layer " << z << "\n";
                    doneFlag.store(true);
                    progressThread.join();
                    close(fdOut); close(fdRaw);
                    return false;
                }
                remaining -= rd; off += rd; dst += rd;
            }
        }

        uint64_t bzSlabStart = zSlabStart / BLOCK_DIM;
        uint64_t bzSlabEnd = (zSlabEnd + BLOCK_DIM - 1) / BLOCK_DIM;
        if (bzSlabEnd > bzCount) bzSlabEnd = bzCount;
        uint64_t bzSlabCount = bzSlabEnd - bzSlabStart;
        uint64_t slabBlockCount = bzSlabCount * byCount * bxCount;

        // Flat compressed buffer for this slab in Z-order (bz, by, bx)
        std::vector<uint8_t> compBuf(slabBlockCount * blockCompBytes);

        // Submit one task per (bz, by) row: processes all bx blocks
        struct LocalExcItem {
            uint64_t block_id;
            uint64_t data_off;
            std::array<float, 64> raw;
        };
        std::vector<std::future<std::vector<LocalExcItem>>> futures;
        futures.reserve(bzSlabCount * byCount);

        for (uint64_t bz = bzSlabStart; bz < bzSlabEnd; ++bz) {
            uint64_t lzBase = bz * BLOCK_DIM - zSlabStart;
            for (uint64_t by = 0; by < byCount; ++by) {
                futures.push_back(pool.submit([&, bz, by, lzBase]() {
                    std::vector<LocalExcItem> localExc;
                    float blockOrig[64], blockDec[64];

                    for (uint64_t bx = 0; bx < bxCount; ++bx) {
                        std::memset(blockOrig, 0, BLOCK_BYTES);

                        for (uint64_t iz = 0; iz < BLOCK_DIM; ++iz) {
                            uint64_t localZ = lzBase + iz;
                            if (localZ >= (uint64_t)slabZ) continue;
                            for (uint64_t iy = 0; iy < BLOCK_DIM; ++iy) {
                                uint64_t gy = by * BLOCK_DIM + iy;
                                if (gy >= ny) continue;
                                for (uint64_t ix = 0; ix < BLOCK_DIM; ++ix) {
                                    uint64_t gx = bx * BLOCK_DIM + ix;
                                    if (gx >= nx) continue;
                                    blockOrig[(iz * BLOCK_DIM + iy) * BLOCK_DIM + ix] =
                                        slab[localZ * rowFloats + gy * nx + gx];
                                }
                            }
                        }

                        ZfpWriteState& zfp = getZfp();
                        size_t compBytes = zfp.compress(blockOrig, g_rate, blockDec);

                        uint64_t flatIdx = ((bz - bzSlabStart) * byCount + by) * bxCount + bx;
                        uint8_t* dst = compBuf.data() + flatIdx * blockCompBytes;
                        if (compBytes < blockCompBytes) {
                            std::memcpy(dst, zfp.compBuf.data(), compBytes);
                            std::memset(dst + compBytes, 0, blockCompBytes - compBytes);
                        } else {
                            std::memcpy(dst, zfp.compBuf.data(), blockCompBytes);
                        }

                        bool exc = false;
                        for (uint64_t i = 0; i < BLOCK_FLOATS; ++i) {
                            if (isFailedCheck(blockOrig[i], blockDec[i], relTol, zeroAbsTol)) {
                                exc = true;
                                break;
                            }
                        }
                        if (exc) {
                            uint64_t dataOff = exceptionDataOff.fetch_add(BLOCK_BYTES);
                            uint64_t globalBlockId = (bz * byCount + by) * bxCount + bx;
                            exceptionCount.fetch_add(1);
                            LocalExcItem item;
                            item.block_id = globalBlockId;
                            item.data_off = dataOff;
                            std::memcpy(item.raw.data(), blockOrig, BLOCK_BYTES);
                            localExc.push_back(item);
                        }

                        blocksCompressed.fetch_add(1);
                    }
                    return localExc;
                }));
            }
        }

        // Collect exception data from tasks
        std::vector<std::pair<uint64_t, std::vector<uint8_t>>> excEntries;
        for (auto& f : futures) {
            auto localExc = f.get();
            for (auto& e : localExc) {
                std::vector<uint8_t> raw(BLOCK_BYTES);
                std::memcpy(raw.data(), e.raw.data(), BLOCK_BYTES);
                excEntries.emplace_back(e.data_off, std::move(raw));
                excPairs.push_back({e.block_id, e.data_off});
            }
        }

        // Write Z-axis slab: compBuf is already in Z-order
        uint64_t zSlabFileOff = axisOffsets[2] + bzSlabStart * blocksPerOuter[2] * blockCompBytes;
        pwrite(fdOut, compBuf.data(), compBuf.size(), zSlabFileOff);

        // Write Y-axis slabs: for each by, gather all bz_local rows (contiguous in file)
        std::vector<uint8_t> yGatherBuf(bzSlabCount * bxCount * blockCompBytes);
        for (uint64_t by = 0; by < byCount; ++by) {
            for (uint64_t bzLocal = 0; bzLocal < bzSlabCount; ++bzLocal) {
                uint64_t srcFlatIdx = (bzLocal * byCount + by) * bxCount;
                uint8_t* dst = yGatherBuf.data() + bzLocal * bxCount * blockCompBytes;
                std::memcpy(dst, compBuf.data() + srcFlatIdx * blockCompBytes,
                            bxCount * blockCompBytes);
            }
            uint64_t yFileOff = axisOffsets[1] + (by * bzCount + bzSlabStart) * bxCount * blockCompBytes;
            pwrite(fdOut, yGatherBuf.data(), yGatherBuf.size(), yFileOff);
        }

        // Write X-axis slabs: for each bx, gather all (bz_local, by) rows (contiguous in file)
        std::vector<uint8_t> xGatherBuf(bzSlabCount * byCount * blockCompBytes);
        for (uint64_t bx = 0; bx < bxCount; ++bx) {
            for (uint64_t bzLocal = 0; bzLocal < bzSlabCount; ++bzLocal) {
                for (uint64_t by = 0; by < byCount; ++by) {
                    uint64_t srcFlatIdx = (bzLocal * byCount + by) * bxCount + bx;
                    uint8_t* dst = xGatherBuf.data() +
                                   (bzLocal * byCount + by) * blockCompBytes;
                    std::memcpy(dst, compBuf.data() + srcFlatIdx * blockCompBytes, blockCompBytes);
                }
            }
            uint64_t xFileOff = axisOffsets[0] + (bx * bzCount + bzSlabStart) * byCount * blockCompBytes;
            pwrite(fdOut, xGatherBuf.data(), xGatherBuf.size(), xFileOff);
        }

        // Write exception raw data for this slab (sorted + merged)
        if (!excEntries.empty()) {
            std::sort(excEntries.begin(), excEntries.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
            writeSortedEntries(fdOut, excEntries);
        }

        {
            std::lock_guard<std::mutex> lock(progressMutex);
            double elapsed = std::chrono::duration<double>(
                std::chrono::high_resolution_clock::now() - tStart).count();
            std::cerr << "\r  slab " << (slabIdx + 1) << "/" << totalSlabs
                      << " done (" << blocksCompressed.load() << "/" << totalBlocks
                      << " blocks), elapsed " << std::fixed << std::setprecision(1)
                      << elapsed << "s                              " << std::endl;
        }
    }

    doneFlag.store(true);
    progressThread.join();

    uint64_t excCount = exceptionCount.load();
    std::cerr << "\n  Exceptions: " << excCount << " / " << totalBlocks
              << " (" << (100.0 * excCount / totalBlocks) << "%)\n";

    if (excCount > 0) {
        std::sort(excPairs.begin(), excPairs.end(),
            [](const TriExceptionIndex& a, const TriExceptionIndex& b) {
                return a.block_id < b.block_id;
            });
        pwrite(fdOut, excPairs.data(),
               excPairs.size() * sizeof(TriExceptionIndex),
               exceptionIndexStart);
    }

    for (int i = 0; i < 3; ++i) {
        header.axis_offsets[i] = axisOffsets[i];
        header.axis_block_counts[i] = axisBlockCounts[i];
        header.axis_block_bytes[i] = blockCompBytes;
    }
    header.exception_index_offset = excCount > 0 ? exceptionIndexStart : 0;
    header.exception_data_offset = excCount > 0 ? exceptionDataStart : 0;
    header.exception_count = excCount;
    if (excCount > 0) header.flags |= TRI_FLAG_HAS_EXCEPTION;

    pwrite(fdOut, &header, sizeof(TriHeader), 0);

    close(fdOut);
    close(fdRaw);

    uint64_t finalDataEnd = exceptionDataStart + excCount * BLOCK_BYTES;
    int fdTrim = open(outPath.c_str(), O_RDWR);
    if (fdTrim >= 0) {
        ftruncate(fdTrim, finalDataEnd);
        close(fdTrim);
    }

    auto tEnd = std::chrono::high_resolution_clock::now();
    double totalSec = std::chrono::duration<double>(tEnd - tStart).count();

    double rawBytes = (double)(nx * ny * nz * 4);
    struct stat st;
    stat(outPath.c_str(), &st);
    double fileSize = (double)st.st_size;
    double ratio = fileSize / rawBytes;

    std::cerr << "\n  Done in " << totalSec << "s\n";
    std::cerr << "  File size: " << (fileSize / 1e9) << " GB\n";
    std::cerr << "  Storage ratio: " << ratio << "x\n";
    std::cerr << "  Exceptions: " << excCount << " (" << (100.0 * excCount / totalBlocks) << "%)\n";

    return true;
}

#else // !ERWT3D_HAVE_ZFP

bool writeTriAxisLayout(const std::string&, const std::string&,
                        uint64_t, uint64_t, uint64_t,
                        uint32_t, uint32_t,
                        double, double,
                        int, size_t) {
    std::cerr << "Error: ZFP support not compiled in\n";
    return false;
}

#endif

} // namespace erwt3d
