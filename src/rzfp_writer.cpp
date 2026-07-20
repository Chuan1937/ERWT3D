#include "erwt3d/rzfp_writer.hpp"
#include "erwt3d/morton.hpp"
#include "erwt3d/raw_layout.hpp"
#include "erwt3d/raw_x_aux.hpp"
#include "erwt3d/thread_pool.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>
#include "erwt3d/platform_io.hpp"
#include <vector>

namespace erwt3d {

namespace {

static void packLeavesFromSlab(
    uint8_t* out,
    const RzfpFileHeader& header,
    const float* slab,
    uint64_t nx, uint64_t ny, uint64_t nz,
    uint64_t xStart, uint64_t currentX,
    uint64_t sx, uint64_t sy, uint64_t sz
) {
    const uint64_t leafBytes = sizeof(float) * header.leaf_x * header.leaf_y * header.leaf_z;
    const uint64_t validX = std::min<uint64_t>(header.super_x, nx - sx * header.super_x);
    const uint64_t validY = std::min<uint64_t>(header.super_y, ny - sy * header.super_y);
    const uint64_t validZ = std::min<uint64_t>(header.super_z, nz - sz * header.super_z);
    const bool boundary = validX != header.super_x || validY != header.super_y || validZ != header.super_z;
    if (boundary) std::memset(out, 0, header.super_x * header.super_y * header.super_z * sizeof(float));

    const uint64_t leafCount = (header.super_x / header.leaf_x) *
                               (header.super_y / header.leaf_y) *
                               (header.super_z / header.leaf_z);
    const uint64_t leafX = header.leaf_x;
    const uint64_t leafY = header.leaf_y;
    const uint64_t leafZ = header.leaf_z;

    for (uint64_t j = 0; j < leafCount; ++j) {
        uint32_t lx, ly, lz;
        unmorton3D(j, lx, ly, lz);
        const uint64_t bx = static_cast<uint64_t>(lx) * leafX;
        const uint64_t by = static_cast<uint64_t>(ly) * leafY;
        const uint64_t bz = static_cast<uint64_t>(lz) * leafZ;
        if (bx >= validX || by >= validY || bz >= validZ) continue;

        const uint64_t copyX = std::min<uint64_t>(leafX, validX - bx);
        const uint64_t copyY = std::min<uint64_t>(leafY, validY - by);
        const uint64_t copyZ = std::min<uint64_t>(leafZ, validZ - bz);
        float* dst = reinterpret_cast<float*>(out + j * leafBytes);
        for (uint64_t z = 0; z < copyZ; ++z) {
            for (uint64_t y = 0; y < copyY; ++y) {
                const uint64_t dstElem = (z * leafY + y) * leafX;
                for (uint64_t x = 0; x < copyX; ++x) {
                    const uint64_t gx_local = bx + x;
                    const uint64_t gy = sy * header.super_y + by + y;
                    const uint64_t gz = sz * header.super_z + bz + z;
                    const uint64_t srcElem = rawOffsetZFastest(gx_local, gy, gz, ny, nz);
                    dst[dstElem + x] = slab[srcElem];
                }
            }
        }
    }
}

static uint64_t buildValidMask(
    uint64_t start_x, uint64_t start_y, uint64_t start_z,
    uint64_t nx, uint64_t ny, uint64_t nz
) {
    uint64_t mask = 0;
    for (uint32_t z = 0; z < 4; ++z) {
        for (uint32_t y = 0; y < 4; ++y) {
            for (uint32_t x = 0; x < 4; ++x) {
                const uint32_t i = (z * 4 + y) * 4 + x;
                if (start_x + x < nx && start_y + y < ny && start_z + z < nz) {
                    mask |= uint64_t{1} << i;
                }
            }
        }
    }
    return mask;
}

struct ChunkResult {
    std::vector<RzfpLeafDescriptor> descriptors;
    std::vector<uint8_t> payload;
    RzfpWriterStats local_stats{};
    uint32_t leaf_count = 0;
};

static ChunkResult encodeLeafChunk(
    const uint8_t* super_buffer,
    uint64_t leaf_start,
    uint32_t leaf_count,
    uint64_t base_x, uint64_t base_y, uint64_t base_z,
    uint64_t nx, uint64_t ny, uint64_t nz,
    const RzfpCodecConfig& codec_cfg
) {
    ChunkResult result;
    result.descriptors.resize(leaf_count);
    result.leaf_count = leaf_count;
    RzfpCodec codec;
    const uint64_t leafBytes = 4 * 4 * 4 * sizeof(float);

    for (uint32_t k = 0; k < leaf_count; ++k) {
        const uint64_t morton = leaf_start + k;
        const float* leaf_src = reinterpret_cast<const float*>(super_buffer + morton * leafBytes);
        float input[64];
        std::memcpy(input, leaf_src, leafBytes);

        uint32_t lx, ly, lz;
        unmorton3D(static_cast<uint32_t>(morton), lx, ly, lz);
        const uint64_t start_x = base_x + lx * 4;
        const uint64_t start_y = base_y + ly * 4;
        const uint64_t start_z = base_z + lz * 4;
        const uint64_t valid_mask = buildValidMask(start_x, start_y, start_z, nx, ny, nz);

        RzfpCandidate cand = codec.encodeBest(input, valid_mask, codec_cfg);

        ++result.local_stats.total_leaves;
        result.local_stats.total_exceptions += cand.exception_count;
        result.local_stats.max_exceptions = std::max(
            result.local_stats.max_exceptions, cand.exception_count);
        result.local_stats.max_relative_error = std::max(
            result.local_stats.max_relative_error, cand.error_stats.max_relative_error);
        result.local_stats.violation_count += cand.error_stats.violation_count;

        switch (cand.codec) {
            case RzfpLeafCodec::RawFloat32: ++result.local_stats.raw_leaves; break;
            case RzfpLeafCodec::ConstantZero: ++result.local_stats.zero_leaves; break;
            case RzfpLeafCodec::ConstantValue: ++result.local_stats.constant_leaves; break;
            case RzfpLeafCodec::ZfpAccuracy: ++result.local_stats.accuracy_leaves; break;
            case RzfpLeafCodec::ZfpAccuracyExceptions: ++result.local_stats.accuracy_exception_leaves; break;
            case RzfpLeafCodec::ZfpPrecision: ++result.local_stats.precision_leaves; break;
        }

        result.descriptors[k] = makeDescriptor(cand.codec, static_cast<uint16_t>(cand.payload.size()));
        const size_t off = result.payload.size();
        result.payload.resize(off + cand.payload.size());
        std::memcpy(result.payload.data() + off, cand.payload.data(), cand.payload.size());
    }

    return result;
}

} // namespace

bool writeRzfpFile(
    const std::string& raw_path,
    const std::string& output_path,
    const RzfpWriterConfig& config,
    RzfpWriterStats* out_stats
) {
    using Clock = std::chrono::steady_clock;

    RzfpFileHeader header;
    initRzfpHeader(header);
    header.nx = config.nx;
    header.ny = config.ny;
    header.nz = config.nz;
    header.super_x = config.super_size;
    header.super_y = config.super_size;
    header.super_z = config.super_size;
    header.leaf_x = config.leaf_size;
    header.leaf_y = config.leaf_size;
    header.leaf_z = config.leaf_size;
    if (config.physical_order == PhysicalOrder::V05_YZX) {
        header.flags |= FLAG_PHYSICAL_ORDER_YZX;
    }

    const uint64_t sgX = rzfpSuperGridX(header);
    const uint64_t sgY = rzfpSuperGridY(header);
    const uint64_t sgZ = rzfpSuperGridZ(header);
    const uint64_t totalSB = sgX * sgY * sgZ;
    const uint64_t leavesPerSB = rzfpTotalLeafsPerSuper(header);
    const uint64_t totalLeaves = totalSB * leavesPerSB;
    const uint64_t rawSize = rzfpRawSize(header);

    const uint64_t indexBytes = totalSB * sizeof(RzfpSuperblockIndex);
    const uint64_t descriptorBytes = totalLeaves * sizeof(RzfpLeafDescriptor);
    const uint64_t payloadStart = sizeof(RzfpFileHeader) + indexBytes + descriptorBytes;

    header.descriptor_offset = sizeof(RzfpFileHeader) + indexBytes;
    header.payload_offset = payloadStart;

    int rawFd = io_open(raw_path.c_str(), O_RDONLY);
    if (rawFd < 0) {
        std::cerr << "Error: cannot open raw file: " << raw_path << std::endl;
        return false;
    }

    const std::string tmpPath = output_path + ".tmp";
    int outFd = io_open(tmpPath.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (outFd < 0) {
        std::cerr << "Error: cannot create output file: " << tmpPath << std::endl;
        io_close(rawFd);
        return false;
    }

    if (posix_fallocate(outFd, 0, static_cast<int64_t>(payloadStart)) != 0) {
        if (ftruncate(outFd, static_cast<int64_t>(payloadStart)) != 0) {
            std::cerr << "Error: failed to reserve output header/index/descriptor area" << std::endl;
            io_close(rawFd); io_close(outFd); unlink(tmpPath.c_str());
            return false;
        }
    }

    if (!writeFullyAt(outFd, &header, sizeof(header), 0)) {
        std::cerr << "Error: failed to write header" << std::endl;
        io_close(rawFd); io_close(outFd); unlink(tmpPath.c_str());
        return false;
    }

    const uint64_t rawYZFloats = config.ny * config.nz;
    const uint64_t rawYZBytes = rawYZFloats * sizeof(float);
    const uint64_t slabX = config.super_size;
    const uint64_t slabBytes = rawYZBytes * slabX;

    std::vector<float> slab[2];
    slab[0].resize(slabBytes / sizeof(float));
    slab[1].resize(slabBytes / sizeof(float));
    std::vector<uint8_t> leafBuffer(header.super_x * header.super_y * header.super_z * sizeof(float));
    std::vector<RzfpSuperblockIndex> sbIndex(totalSB);

    ThreadPool pool(static_cast<size_t>(std::max(1, config.threads)));
    RzfpWriterStats stats{};
    double totalIoMs = 0.0;
    double totalReadMs = 0.0;

    uint64_t currentPayloadOffset = payloadStart;

    auto readSlab = [&](uint64_t bufIdx, uint64_t xStart) -> bool {
        const uint64_t currentX = std::min<uint64_t>(slabX, config.nx - xStart);
        const uint64_t currentBytes = currentX * rawYZBytes;
        return readFullyAt(rawFd, slab[bufIdx].data(), currentBytes, xStart * rawYZBytes);
    };

    std::future<bool> readFuture;
    if (!readSlab(0, 0)) {
        std::cerr << "Error reading initial raw X-slab" << std::endl;
        io_close(rawFd); io_close(outFd); unlink(tmpPath.c_str());
        return false;
    }

    int curBuf = 0;
    for (uint64_t xStart = 0; xStart < config.nx; xStart += slabX) {
        const uint64_t currentX = std::min<uint64_t>(slabX, config.nx - xStart);
        const uint64_t nextStart = xStart + slabX;
        const uint64_t sx = xStart / slabX;

        if (nextStart < config.nx) {
            readFuture = std::async(std::launch::async, readSlab, 1 - curBuf, nextStart);
        }

        auto t0 = Clock::now();
        for (uint64_t sz = 0; sz < sgZ; ++sz) {
            for (uint64_t sy = 0; sy < sgY; ++sy) {
                packLeavesFromSlab(
                    leafBuffer.data(), header, slab[curBuf].data(), config.nx, config.ny, config.nz,
                    xStart, currentX, sx, sy, sz);

                const uint64_t sbId = rzfpSuperblockId(header, sz, sy, sx, config.physical_order);
                const uint64_t base_x = sx * header.super_x;
                const uint64_t base_y = sy * header.super_y;
                const uint64_t base_z = sz * header.super_z;

                std::vector<std::future<ChunkResult>> futures;
                const uint32_t chunkLeaves = 64;
                const uint32_t numChunks = static_cast<uint32_t>(leavesPerSB / chunkLeaves);
                for (uint32_t c = 0; c < numChunks; ++c) {
                    futures.push_back(pool.submit(encodeLeafChunk,
                        leafBuffer.data(),
                        static_cast<uint64_t>(c) * chunkLeaves,
                        chunkLeaves,
                        base_x, base_y, base_z,
                        config.nx, config.ny, config.nz,
                        config.codec));
                }

                std::vector<RzfpLeafDescriptor> descriptors(leavesPerSB);
                std::vector<uint8_t> payload;

                for (uint32_t c = 0; c < numChunks; ++c) {
                    ChunkResult cr = futures[c].get();
                    std::memcpy(descriptors.data() + c * chunkLeaves,
                                cr.descriptors.data(), cr.descriptors.size() * sizeof(RzfpLeafDescriptor));
                    const size_t oldSize = payload.size();
                    payload.resize(oldSize + cr.payload.size());
                    std::memcpy(payload.data() + oldSize, cr.payload.data(), cr.payload.size());

                    stats.total_leaves += cr.local_stats.total_leaves;
                    stats.raw_leaves += cr.local_stats.raw_leaves;
                    stats.zero_leaves += cr.local_stats.zero_leaves;
                    stats.constant_leaves += cr.local_stats.constant_leaves;
                    stats.accuracy_leaves += cr.local_stats.accuracy_leaves;
                    stats.accuracy_exception_leaves += cr.local_stats.accuracy_exception_leaves;
                    stats.precision_leaves += cr.local_stats.precision_leaves;
                    stats.total_exceptions += cr.local_stats.total_exceptions;
                    stats.max_exceptions = std::max(stats.max_exceptions, cr.local_stats.max_exceptions);
                    stats.max_relative_error = std::max(stats.max_relative_error, cr.local_stats.max_relative_error);
                    stats.violation_count += cr.local_stats.violation_count;
                }

                if (stats.violation_count > 0) {
                    std::cerr << "Error: violations detected during write, aborting" << std::endl;
                    io_close(rawFd); io_close(outFd); unlink(tmpPath.c_str());
                    return false;
                }

                const uint64_t descOff = rzfpSuperblockDescriptorOffset(header, sbId);
                auto t0 = Clock::now();
                if (!writeFullyAt(outFd, descriptors.data(), descriptors.size() * sizeof(RzfpLeafDescriptor), descOff)) {
                    std::cerr << "Error writing descriptors for sb=" << sbId << std::endl;
                    io_close(rawFd); io_close(outFd); unlink(tmpPath.c_str());
                    return false;
                }
                if (!payload.empty()) {
                    if (!writeFullyAt(outFd, payload.data(), payload.size(), currentPayloadOffset)) {
                        std::cerr << "Error writing payload for sb=" << sbId << std::endl;
                        io_close(rawFd); io_close(outFd); unlink(tmpPath.c_str());
                        return false;
                    }
                }
                auto t1 = Clock::now();
                totalIoMs += std::chrono::duration<double, std::milli>(t1 - t0).count();

                sbIndex[sbId].payload_offset = currentPayloadOffset;
                sbIndex[sbId].payload_bytes = static_cast<uint32_t>(payload.size());
                currentPayloadOffset += payload.size();

                std::cout << "\rRZFP write progress: " << std::fixed << std::setprecision(1)
                          << (100.0 * std::min<uint64_t>(xStart + currentX, config.nx) / config.nx)
                          << "%" << std::flush;
            }
        }

        if (readFuture.valid()) {
            auto rt0 = Clock::now();
            if (!readFuture.get()) {
                std::cerr << "Error reading raw X-slab at x=" << nextStart << std::endl;
                io_close(rawFd); io_close(outFd); unlink(tmpPath.c_str());
                return false;
            }
            auto rt1 = Clock::now();
            totalReadMs += std::chrono::duration<double, std::milli>(rt1 - rt0).count();
        }
        curBuf = 1 - curBuf;
    }
    std::cout << std::endl;

    io_close(rawFd);

    auto t0 = Clock::now();
    if (!writeFullyAt(outFd, sbIndex.data(), indexBytes, sizeof(RzfpFileHeader))) {
        std::cerr << "Error writing superblock index" << std::endl;
        io_close(outFd); unlink(tmpPath.c_str());
        return false;
    }
    if (!writeFullyAt(outFd, &header, sizeof(header), 0)) {
        std::cerr << "Error finalizing header" << std::endl;
        io_close(outFd); unlink(tmpPath.c_str());
        return false;
    }
    auto t1 = Clock::now();
    totalIoMs += std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (fsync(outFd) != 0) {
        std::cerr << "Warning: fsync failed" << std::endl;
    }
    io_close(outFd);

    if (rename(tmpPath.c_str(), output_path.c_str()) != 0) {
        std::cerr << "Error renaming " << tmpPath << " to " << output_path << std::endl;
        unlink(tmpPath.c_str());
        return false;
    }

    stats.payload_bytes = currentPayloadOffset - payloadStart;
    stats.descriptor_bytes = descriptorBytes;
    stats.index_bytes = indexBytes;
    stats.storage_ratio = static_cast<double>(currentPayloadOffset) / static_cast<double>(rawSize);
    stats.average_exceptions_per_leaf = stats.total_leaves > 0
        ? static_cast<double>(stats.total_exceptions) / stats.total_leaves
        : 0.0;

    std::cout << "RZFP write complete: total_leaves=" << stats.total_leaves
              << ", payload_bytes=" << stats.payload_bytes
              << ", storage_ratio=" << stats.storage_ratio
              << ", raw_leaves=" << stats.raw_leaves
              << ", zero_leaves=" << stats.zero_leaves
              << ", constant_leaves=" << stats.constant_leaves
              << ", accuracy_leaves=" << stats.accuracy_leaves
              << ", accuracy_exception_leaves=" << stats.accuracy_exception_leaves
              << ", precision_leaves=" << stats.precision_leaves
              << ", violations=" << stats.violation_count
              << std::endl;

    if (out_stats) *out_stats = stats;
    return true;
}

bool appendRawXAuxToRzfpFile(
    const std::string& rzfpPath,
    const std::string& rawPath,
    uint64_t nx, uint64_t ny, uint64_t nz,
    RawXAuxStats* stats, bool forceEdge) {
    if (stats) *stats = RawXAuxStats{};

    ScopedFd rzfpFd(io_open(rzfpPath.c_str(), O_RDWR));
    if (!rzfpFd.valid()) {
        std::cerr << "Error: Cannot open RZFP file for raw X aux append: " << rzfpPath << std::endl;
        setRawXAuxFailure(stats, "cannot open RZFP file");
        return false;
    }

    RzfpFileHeader header;
    if (!readFullyAt(rzfpFd.get(), &header, sizeof(header), 0)) {
        std::cerr << "Error: Cannot read RZFP header from " << rzfpPath << std::endl;
        setRawXAuxFailure(stats, "header read failed");
        return false;
    }

    if (hasRawXAux(header)) {
        if (stats) {
            stats->status = RawXAuxStatus::AlreadyPresent;
            stats->raw_x_aux_offset = rzfpRawXAuxOffset(header);
            stats->raw_x_aux_bytes = rzfpRawXAuxBytes(header);
            stats->raw_x_aux_plane_bytes = rzfpRawXAuxPlaneBytes(header);
            stats->raw_x_aux_version = rzfpRawXAuxVersion(header);
            stats->message = "already present";
        }
        return true;
    }

    struct _stat64 st;
    if (fstat(rzfpFd.get(), &st) != 0) {
        std::cerr << "Error: cannot stat " << rzfpPath << std::endl;
        setRawXAuxFailure(stats, "stat failed");
        return false;
    }
    const uint64_t mainFileBytes = static_cast<uint64_t>(st.st_size);

    ScopedFd rawFd(io_open(rawPath.c_str(), O_RDONLY));
    if (!rawFd.valid()) {
        std::cerr << "Error: Cannot open raw file: " << rawPath << std::endl;
        setRawXAuxFailure(stats, "cannot open raw file");
        return false;
    }

    struct _stat64 rawSt;
    if (fstat(rawFd.get(), &rawSt) != 0) {
        std::cerr << "Error: cannot stat raw file" << std::endl;
        setRawXAuxFailure(stats, "raw file stat failed");
        return false;
    }
    const uint64_t rawBytes = static_cast<uint64_t>(rawSt.st_size);

    uint64_t expectedPlaneFloats = 0, planeBytes = 0, rawAuxBytes = 0;
    if (!checkedMulU64(ny, nz, expectedPlaneFloats) ||
        !checkedMulU64(expectedPlaneFloats, sizeof(float), planeBytes) ||
        !checkedMulU64(nx, planeBytes, rawAuxBytes)) {
        std::cerr << "Error: RZFP raw X auxiliary size overflow" << std::endl;
        setRawXAuxFailure(stats, "size overflow");
        return false;
    }

    if (rawBytes != rawAuxBytes) {
        std::cerr << "Error: raw file size " << rawBytes
                  << " does not match nx*ny*nz*sizeof(float) = " << rawAuxBytes << std::endl;
        setRawXAuxFailure(stats, "raw file size mismatch");
        return false;
    }

    uint64_t alignedOffset = 0;
    if (!checkedAddU64(mainFileBytes, RAW_X_AUX_ALIGN - 1, alignedOffset)) {
        setRawXAuxFailure(stats, "aligned offset overflow");
        return false;
    }
    alignedOffset &= ~(RAW_X_AUX_ALIGN - 1);

    uint64_t totalProjected = 0;
    if (!checkedAddU64(alignedOffset, rawAuxBytes, totalProjected)) {
        setRawXAuxFailure(stats, "projected size overflow");
        return false;
    }

    double projectedRatio = static_cast<double>(totalProjected) / static_cast<double>(rawBytes);
    if (!std::isfinite(projectedRatio)) {
        setRawXAuxFailure(stats, "invalid projected ratio");
        return false;
    }

    if (rawBytes >= RAW_X_AUX_MIN_RAW_BYTES_FOR_RATIO_CHECK) {
        if (projectedRatio > RAW_X_AUX_HARD_LIMIT) {
            std::cerr << "Error: projected storage ratio " << std::fixed << std::setprecision(3)
                      << projectedRatio << "x exceeds absolute hard limit "
                      << RAW_X_AUX_HARD_LIMIT << "x. Raw X auxiliary rejected." << std::endl;
            if (stats) {
                stats->status = RawXAuxStatus::SkippedStorageBudget;
                stats->total_storage_ratio = projectedRatio;
                stats->message = "hard limit exceeded";
            }
            return false;
        }

        if (projectedRatio > RAW_X_AUX_MAX_RATIO && !forceEdge) {
            std::cerr << "Error: projected storage ratio " << std::fixed << std::setprecision(3)
                      << projectedRatio << "x exceeds safe limit " << RAW_X_AUX_MAX_RATIO
                      << "x. Use --force-storage-edge only for ratios <= "
                      << RAW_X_AUX_HARD_LIMIT << "x." << std::endl;
            if (stats) {
                stats->status = RawXAuxStatus::SkippedStorageBudget;
                stats->total_storage_ratio = projectedRatio;
                stats->message = "soft limit exceeded";
            }
            return false;
        }
    }

    posix_fadvise(rawFd.get(), 0, static_cast<int64_t>(rawBytes), POSIX_FADV_SEQUENTIAL);
    posix_fadvise(rawFd.get(), 0, static_cast<int64_t>(rawBytes), POSIX_FADV_WILLNEED);

    if (posix_fallocate(rzfpFd.get(), static_cast<int64_t>(alignedOffset),
                        static_cast<int64_t>(rawAuxBytes)) != 0) {
        std::cerr << "Warning: posix_fallocate failed, using sequential write" << std::endl;
    }

    FileAppendTransactionWithHeader<RzfpFileHeader> txn(rzfpFd.get(), mainFileBytes, header);

    std::cout << "Appending raw X auxiliary to RZFP: " << (rawAuxBytes / (1024*1024)) << " MB"
              << " (" << nx << " planes x " << (planeBytes / (1024*1024)) << " MB each)"
              << ", total ratio: " << std::fixed << std::setprecision(3) << projectedRatio << "x" << std::endl;

    std::vector<uint8_t> copyBuf(RAW_X_AUX_COPY_CHUNK);
    auto startTime = std::chrono::high_resolution_clock::now();

    for (uint64_t rawOff = 0, outOff = alignedOffset; rawOff < rawAuxBytes; ) {
        uint64_t chunk = std::min(rawAuxBytes - rawOff, RAW_X_AUX_COPY_CHUNK);
        if (!readFullyAt(rawFd.get(), copyBuf.data(), chunk, rawOff)) {
            std::cerr << "Error reading raw data at offset " << rawOff << std::endl;
            setRawXAuxFailure(stats, "raw read failed");
            return false;
        }
        if (!writeFullyAt(rzfpFd.get(), copyBuf.data(), chunk, outOff)) {
            std::cerr << "Error writing raw X aux data at offset " << outOff << std::endl;
            setRawXAuxFailure(stats, "payload write failed");
            return false;
        }
        rawOff += chunk; outOff += chunk;

        if (rawOff % (RAW_X_AUX_COPY_CHUNK * 4) == 0 || rawOff >= rawAuxBytes) {
            auto now = std::chrono::high_resolution_clock::now();
            double elapsed = std::chrono::duration<double>(now - startTime).count();
            double pct = 100.0 * static_cast<double>(rawOff) / static_cast<double>(rawAuxBytes);
            double speed = elapsed > 0 ? (rawOff / (1024.0 * 1024.0)) / elapsed : 0.0;
            std::cout << "\rRaw X aux progress: " << std::fixed << std::setprecision(1)
                      << pct << "% (" << (rawOff / (1024*1024)) << " MB)"
                      << " " << std::setprecision(1) << speed << " MB/s" << std::flush;
        }
    }
    std::cout << std::endl;

    if (fdatasync(rzfpFd.get()) != 0) {
        std::cerr << "Error: fdatasync payload failed" << std::endl;
        setRawXAuxFailure(stats, "fdatasync payload failed");
        return false;
    }

    RzfpFileHeader newHeader = header;
    newHeader.flags |= FLAG_HAS_RAW_X_AUX;
    newHeader.reserved[0] = alignedOffset;
    newHeader.reserved[1] = rawAuxBytes;
    newHeader.reserved[2] = planeBytes;
    newHeader.reserved[3] = RAW_X_AUX_VERSION;

    if (!writeFullyAt(rzfpFd.get(), &newHeader, sizeof(newHeader), 0)) {
        std::cerr << "Error: failed to write back RZFP header with raw X aux metadata" << std::endl;
        setRawXAuxFailure(stats, "header write failed");
        return false;
    }

    if (fsync(rzfpFd.get()) != 0) {
        std::cerr << "Error: fsync header failed" << std::endl;
        setRawXAuxFailure(stats, "header fsync failed");
        return false;
    }

    txn.commit();

    auto endTime = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(endTime - startTime).count();
    std::cout << "Raw X auxiliary complete: " << (rawAuxBytes / (1024*1024))
              << " MB in " << std::fixed << std::setprecision(1) << elapsed << "s"
              << " (" << std::setprecision(1) << (rawAuxBytes / (1024.0*1024.0) / elapsed) << " MB/s)"
              << std::endl;

    if (stats) {
        stats->status = RawXAuxStatus::Stored;
        stats->raw_x_aux_offset = alignedOffset;
        stats->raw_x_aux_bytes = rawAuxBytes;
        stats->raw_x_aux_plane_bytes = planeBytes;
        stats->raw_x_aux_version = RAW_X_AUX_VERSION;
        stats->total_storage_ratio = projectedRatio;
    }
    return true;
}

} // namespace erwt3d
