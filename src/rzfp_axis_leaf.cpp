#include "erwt3d/rzfp_axis_leaf.hpp"
#include "erwt3d/morton.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace erwt3d {

namespace {

constexpr uint64_t KiB = 1024ULL;
constexpr uint64_t MiB = 1024ULL * KiB;

bool readFullyAt(int fd, void* dst, uint64_t bytes, uint64_t offset) {
    auto* out = static_cast<uint8_t*>(dst);
    uint64_t done = 0;
    while (done < bytes) {
        const ssize_t n = pread(
            fd,
            out + done,
            static_cast<size_t>(bytes - done),
            static_cast<off_t>(offset + done)
        );
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += static_cast<uint64_t>(n);
    }
    return true;
}

bool writeFullyAt(int fd, const void* src, uint64_t bytes, uint64_t offset) {
    const auto* in = static_cast<const uint8_t*>(src);
    uint64_t done = 0;
    while (done < bytes) {
        const ssize_t n = pwrite(
            fd,
            in + done,
            static_cast<size_t>(bytes - done),
            static_cast<off_t>(offset + done)
        );
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += static_cast<uint64_t>(n);
    }
    return true;
}

bool writeFully(int fd, const void* src, uint64_t bytes) {
    const auto* in = static_cast<const uint8_t*>(src);
    uint64_t done = 0;
    while (done < bytes) {
        const ssize_t n = write(
            fd,
            in + done,
            static_cast<size_t>(bytes - done)
        );
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += static_cast<uint64_t>(n);
    }
    return true;
}

uint64_t descriptorHash(
    const std::vector<RzfpLeafDescriptor>& descriptors
) {
    uint64_t hash = 1469598103934665603ULL;
    const auto* data =
        reinterpret_cast<const uint8_t*>(descriptors.data());
    const uint64_t bytes =
        descriptors.size() * sizeof(RzfpLeafDescriptor);
    for (uint64_t i = 0; i < bytes; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

void physicalSuperblockCoordinates(
    const RzfpFileHeader& header,
    uint64_t physical,
    uint64_t& sx,
    uint64_t& sy,
    uint64_t& sz
) {
    const uint64_t sgX = rzfpSuperGridX(header);
    const uint64_t sgY = rzfpSuperGridY(header);
    const uint64_t sgZ = rzfpSuperGridZ(header);
    sx = physical % sgX;
    const uint64_t rem = physical / sgX;
    if ((header.flags & FLAG_PHYSICAL_ORDER_YZX) != 0) {
        sz = rem % sgZ;
        sy = rem / sgZ;
    } else {
        sy = rem % sgY;
        sz = rem / sgY;
    }
}

uint64_t paddedLeafSlabCount(
    const RzfpFileHeader& header,
    PlaneAxis axis
) {
    switch (axis) {
        case PlaneAxis::X:
            return rzfpSuperGridX(header) *
                   rzfpLeafsPerSuperX(header);
        case PlaneAxis::Y:
            return rzfpSuperGridY(header) *
                   rzfpLeafsPerSuperY(header);
        case PlaneAxis::Z:
            return rzfpSuperGridZ(header) *
                   rzfpLeafsPerSuperZ(header);
    }
    return 0;
}

uint64_t leafSlabForRecord(
    const RzfpFileHeader& header,
    PlaneAxis axis,
    uint64_t physicalSb,
    uint32_t morton
) {
    uint64_t sx = 0;
    uint64_t sy = 0;
    uint64_t sz = 0;
    physicalSuperblockCoordinates(
        header, physicalSb, sx, sy, sz);

    uint32_t lx = 0;
    uint32_t ly = 0;
    uint32_t lz = 0;
    unmorton3D(morton, lx, ly, lz);

    switch (axis) {
        case PlaneAxis::X:
            return sx * rzfpLeafsPerSuperX(header) + lx;
        case PlaneAxis::Y:
            return sy * rzfpLeafsPerSuperY(header) + ly;
        case PlaneAxis::Z:
            return sz * rzfpLeafsPerSuperZ(header) + lz;
    }
    return 0;
}

bool appendBucket(
    const std::filesystem::path& path,
    std::vector<uint8_t>& buffer
) {
    if (buffer.empty()) return true;
    const int fd = open(
        path.c_str(),
        O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
        0644
    );
    if (fd < 0) return false;
    const bool ok = writeFully(fd, buffer.data(), buffer.size());
    close(fd);
    buffer.clear();
    return ok;
}

bool copyFileToFd(
    const std::filesystem::path& path,
    int outputFd,
    std::vector<uint8_t>& copyBuffer,
    uint64_t& bytes
) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) return true;
        return false;
    }
    while (true) {
        const ssize_t n = read(fd, copyBuffer.data(), copyBuffer.size());
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            close(fd);
            return false;
        }
        if (n == 0) break;
        if (!writeFully(
                outputFd,
                copyBuffer.data(),
                static_cast<uint64_t>(n))) {
            close(fd);
            return false;
        }
        bytes += static_cast<uint64_t>(n);
    }
    close(fd);
    return true;
}

bool buildAxisReplica(
    int sourceFd,
    const std::string& outputPath,
    const RzfpFileHeader& header,
    const std::vector<RzfpSuperblockIndex>& sbIndex,
    const std::vector<RzfpLeafDescriptor>& descriptors,
    PlaneAxis axis,
    uint64_t descHash,
    size_t memoryLimitMiB,
    uint64_t& finalBytes
) {
    const uint64_t slabCount = paddedLeafSlabCount(header, axis);
    if (slabCount == 0 ||
        slabCount > std::numeric_limits<uint32_t>::max()) {
        return false;
    }

    const std::string finalPath = rzfpAxisLeafPath(outputPath, axis);
    const std::string tempPath = finalPath + ".tmp";
    const std::filesystem::path bucketDir =
        tempPath + ".buckets";
    std::error_code ec;
    std::filesystem::remove_all(bucketDir, ec);
    ec.clear();
    if (!std::filesystem::create_directories(bucketDir, ec) || ec) {
        return false;
    }

    const uint64_t budgetBytes =
        std::max<uint64_t>(64ULL * MiB,
            static_cast<uint64_t>(memoryLimitMiB) * MiB / 3);
    const uint64_t perSlabBuffer = std::max<uint64_t>(
        64ULL * KiB,
        std::min<uint64_t>(1ULL * MiB, budgetBytes / slabCount)
    );
    std::vector<std::vector<uint8_t>> buffers(
        static_cast<size_t>(slabCount));

    std::vector<uint64_t> physicalOrder(sbIndex.size());
    for (uint64_t i = 0; i < physicalOrder.size(); ++i) {
        physicalOrder[i] = i;
    }
    std::sort(
        physicalOrder.begin(),
        physicalOrder.end(),
        [&](uint64_t a, uint64_t b) {
            return sbIndex[a].payload_offset <
                   sbIndex[b].payload_offset;
        }
    );

    const uint64_t leavesPerSB = rzfpTotalLeafsPerSuper(header);
    std::vector<uint8_t> sbPayload;
    bool ok = true;

    for (uint64_t orderIndex = 0;
         orderIndex < physicalOrder.size() && ok;
         ++orderIndex) {
        const uint64_t physicalSb = physicalOrder[orderIndex];
        const auto& index = sbIndex[physicalSb];
        sbPayload.resize(index.payload_bytes);
        if (!sbPayload.empty() &&
            !readFullyAt(
                sourceFd,
                sbPayload.data(),
                sbPayload.size(),
                index.payload_offset)) {
            ok = false;
            break;
        }

        uint64_t sourceOffset = 0;
        const uint64_t descriptorBase = physicalSb * leavesPerSB;
        for (uint32_t morton = 0;
             morton < leavesPerSB;
             ++morton) {
            const uint64_t descriptorId =
                descriptorBase + morton;
            const uint16_t recordSize =
                descriptorSize(descriptors[descriptorId]);
            if (sourceOffset + recordSize > sbPayload.size() ||
                descriptorId >
                    std::numeric_limits<uint32_t>::max()) {
                ok = false;
                break;
            }

            const uint64_t slab = leafSlabForRecord(
                header, axis, physicalSb, morton);
            if (slab >= slabCount) {
                ok = false;
                break;
            }

            auto& buffer = buffers[static_cast<size_t>(slab)];
            if (buffer.capacity() == 0) {
                buffer.reserve(
                    static_cast<size_t>(perSlabBuffer));
            }
            const uint32_t id =
                static_cast<uint32_t>(descriptorId);
            const size_t oldSize = buffer.size();
            buffer.resize(
                oldSize + sizeof(id) + recordSize);
            std::memcpy(
                buffer.data() + oldSize,
                &id,
                sizeof(id));
            if (recordSize != 0) {
                std::memcpy(
                    buffer.data() + oldSize + sizeof(id),
                    sbPayload.data() + sourceOffset,
                    recordSize);
            }
            sourceOffset += recordSize;

            if (buffer.size() >= perSlabBuffer) {
                const auto bucketPath =
                    bucketDir /
                    (std::to_string(slab) + ".bin");
                if (!appendBucket(bucketPath, buffer)) {
                    ok = false;
                    break;
                }
            }
        }
        if (sourceOffset != sbPayload.size()) ok = false;

        if ((orderIndex + 1) % 128 == 0 ||
            orderIndex + 1 == physicalOrder.size()) {
            std::cout
                << "\rRZFP axis-leaf "
                << axisLabel(axis)
                << " scan: "
                << (100 * (orderIndex + 1) /
                    physicalOrder.size())
                << "%" << std::flush;
        }
    }
    std::cout << std::endl;

    for (uint64_t slab = 0; slab < slabCount && ok; ++slab) {
        const auto bucketPath =
            bucketDir / (std::to_string(slab) + ".bin");
        ok = appendBucket(
            bucketPath,
            buffers[static_cast<size_t>(slab)]);
    }

    int outputFd = -1;
    std::vector<RzfpAxisLeafSlabIndex> slabIndex(
        static_cast<size_t>(slabCount));
    if (ok) {
        outputFd = open(
            tempPath.c_str(),
            O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC,
            0644
        );
        ok = outputFd >= 0;
    }

    RzfpAxisLeafHeader sidecar{};
    if (ok) {
        std::memcpy(
            sidecar.magic,
            RZFP_AXIS_LEAF_MAGIC,
            sizeof(sidecar.magic));
        sidecar.version = RZFP_AXIS_LEAF_VERSION;
        sidecar.axis = static_cast<uint32_t>(axis);
        sidecar.nx = header.nx;
        sidecar.ny = header.ny;
        sidecar.nz = header.nz;
        sidecar.leaf_x = header.leaf_x;
        sidecar.leaf_y = header.leaf_y;
        sidecar.leaf_z = header.leaf_z;
        sidecar.slab_count = slabCount;
        sidecar.index_offset = sizeof(RzfpAxisLeafHeader);
        sidecar.payload_offset =
            sidecar.index_offset +
            slabCount * sizeof(RzfpAxisLeafSlabIndex);
        sidecar.source_payload_bytes = 0;
        for (const auto& index : sbIndex) {
            sidecar.source_payload_bytes +=
                index.payload_bytes;
        }
        sidecar.descriptor_hash = descHash;

        ok = ftruncate(
            outputFd,
            static_cast<off_t>(sidecar.payload_offset)) == 0;
        if (ok) {
            ok = lseek(
                outputFd,
                static_cast<off_t>(sidecar.payload_offset),
                SEEK_SET) >= 0;
        }
    }

    std::vector<uint8_t> copyBuffer(8ULL * MiB);
    uint64_t payloadBytes = 0;
    for (uint64_t slab = 0; slab < slabCount && ok; ++slab) {
        slabIndex[static_cast<size_t>(slab)].offset =
            sidecar.payload_offset + payloadBytes;
        const uint64_t before = payloadBytes;
        const auto bucketPath =
            bucketDir / (std::to_string(slab) + ".bin");
        ok = copyFileToFd(
            bucketPath,
            outputFd,
            copyBuffer,
            payloadBytes);
        slabIndex[static_cast<size_t>(slab)].bytes =
            payloadBytes - before;
    }

    if (ok) {
        ok = writeFullyAt(
            outputFd,
            &sidecar,
            sizeof(sidecar),
            0);
    }
    if (ok) {
        ok = writeFullyAt(
            outputFd,
            slabIndex.data(),
            slabIndex.size() *
                sizeof(RzfpAxisLeafSlabIndex),
            sidecar.index_offset);
    }
    if (ok && fsync(outputFd) != 0) ok = false;
    if (outputFd >= 0) close(outputFd);

    std::filesystem::remove_all(bucketDir, ec);
    if (!ok) {
        unlink(tempPath.c_str());
        return false;
    }
    if (rename(tempPath.c_str(), finalPath.c_str()) != 0) {
        unlink(tempPath.c_str());
        return false;
    }

    finalBytes = sidecar.payload_offset + payloadBytes;
    return true;
}

bool copyMetadataPrefix(
    int sourceFd,
    const std::string& outputPath,
    RzfpFileHeader header,
    uint64_t sourcePayloadBytes,
    uint64_t descHash
) {
    const std::string tempPath = outputPath + ".tmp";
    const int outputFd = open(
        tempPath.c_str(),
        O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC,
        0644
    );
    if (outputFd < 0) return false;

    std::vector<uint8_t> buffer(8ULL * MiB);
    uint64_t copied = 0;
    bool ok = true;
    while (copied < header.payload_offset) {
        const uint64_t chunk = std::min<uint64_t>(
            buffer.size(),
            header.payload_offset - copied);
        if (!readFullyAt(
                sourceFd,
                buffer.data(),
                chunk,
                copied) ||
            !writeFullyAt(
                outputFd,
                buffer.data(),
                chunk,
                copied)) {
            ok = false;
            break;
        }
        copied += chunk;
    }

    header.flags &= ~FLAG_HAS_RAW_X_AUX;
    header.flags |= FLAG_HAS_RZFP_AXIS_LEAF;
    for (uint32_t i = 0; i < 4; ++i) {
        header.reserved[i] = 0;
    }
    header.reserved[4] = RZFP_AXIS_LEAF_VERSION;
    header.reserved[5] = sourcePayloadBytes;
    header.reserved[6] = descHash;

    if (ok) {
        ok = writeFullyAt(
            outputFd,
            &header,
            sizeof(header),
            0);
    }
    if (ok) {
        ok = ftruncate(
            outputFd,
            static_cast<off_t>(header.payload_offset)) == 0;
    }
    if (ok && fsync(outputFd) != 0) ok = false;
    close(outputFd);

    if (!ok) {
        unlink(tempPath.c_str());
        return false;
    }
    if (rename(tempPath.c_str(), outputPath.c_str()) != 0) {
        unlink(tempPath.c_str());
        return false;
    }
    return true;
}

} // namespace

std::string rzfpAxisLeafPath(
    const std::string& metadataPath,
    PlaneAxis axis
) {
    switch (axis) {
        case PlaneAxis::X:
            return metadataPath + ".xal";
        case PlaneAxis::Y:
            return metadataPath + ".yal";
        case PlaneAxis::Z:
            return metadataPath + ".zal";
    }
    return metadataPath + ".al";
}

bool validateRzfpAxisLeafHeader(
    const RzfpAxisLeafHeader& sidecar,
    const RzfpFileHeader& source,
    PlaneAxis axis,
    uint64_t descriptorHashValue
) {
    if (std::memcmp(
            sidecar.magic,
            RZFP_AXIS_LEAF_MAGIC,
            sizeof(sidecar.magic)) != 0) {
        return false;
    }
    if (sidecar.version != RZFP_AXIS_LEAF_VERSION ||
        sidecar.axis != static_cast<uint32_t>(axis)) {
        return false;
    }
    if (sidecar.nx != source.nx ||
        sidecar.ny != source.ny ||
        sidecar.nz != source.nz ||
        sidecar.leaf_x != source.leaf_x ||
        sidecar.leaf_y != source.leaf_y ||
        sidecar.leaf_z != source.leaf_z) {
        return false;
    }
    if (sidecar.slab_count !=
        paddedLeafSlabCount(source, axis)) {
        return false;
    }
    if (sidecar.index_offset !=
            sizeof(RzfpAxisLeafHeader) ||
        sidecar.payload_offset !=
            sidecar.index_offset +
            sidecar.slab_count *
                sizeof(RzfpAxisLeafSlabIndex)) {
        return false;
    }
    return sidecar.descriptor_hash ==
           descriptorHashValue;
}

bool repackRzfpAxisLeaves(
    const std::string& inputPath,
    const std::string& outputPath,
    size_t memoryLimitMiB,
    RzfpAxisLeafRepackStats* outputStats
) {
    if (inputPath == outputPath) {
        std::cerr
            << "Error: axis-leaf repack requires a distinct output path\n";
        return false;
    }

    const int sourceFd = open(
        inputPath.c_str(),
        O_RDONLY | O_CLOEXEC);
    if (sourceFd < 0) return false;

    struct stat sourceStat{};
    RzfpFileHeader header{};
    bool ok =
        fstat(sourceFd, &sourceStat) == 0 &&
        readFullyAt(
            sourceFd,
            &header,
            sizeof(header),
            0) &&
        validateRzfpHeader(header) &&
        !hasRzfpAxisLeaf(header);

    const uint64_t totalSB =
        ok ? rzfpTotalSuperblocks(header) : 0;
    const uint64_t totalLeaves =
        ok ? rzfpTotalLeaves(header) : 0;
    const uint64_t indexBytes =
        totalSB * sizeof(RzfpSuperblockIndex);
    const uint64_t descriptorBytes =
        totalLeaves * sizeof(RzfpLeafDescriptor);

    if (ok &&
        (header.descriptor_offset <
             sizeof(RzfpFileHeader) + indexBytes ||
         header.payload_offset <
             header.descriptor_offset + descriptorBytes ||
         header.payload_offset >
             static_cast<uint64_t>(sourceStat.st_size))) {
        ok = false;
    }

    std::vector<RzfpSuperblockIndex> sbIndex(
        static_cast<size_t>(totalSB));
    std::vector<RzfpLeafDescriptor> descriptors(
        static_cast<size_t>(totalLeaves));
    if (ok) {
        ok = readFullyAt(
            sourceFd,
            sbIndex.data(),
            indexBytes,
            sizeof(RzfpFileHeader));
    }
    if (ok) {
        ok = readFullyAt(
            sourceFd,
            descriptors.data(),
            descriptorBytes,
            header.descriptor_offset);
    }

    uint64_t sourcePayloadBytes = 0;
    if (ok) {
        for (uint64_t sb = 0; sb < totalSB; ++sb) {
            const auto& index = sbIndex[sb];
            if (index.payload_offset < header.payload_offset ||
                index.payload_offset >
                    static_cast<uint64_t>(sourceStat.st_size) ||
                index.payload_bytes >
                    static_cast<uint64_t>(sourceStat.st_size) -
                        index.payload_offset) {
                ok = false;
                break;
            }
            sourcePayloadBytes += index.payload_bytes;
        }
    }

    const uint64_t descHash = ok
        ? descriptorHash(descriptors)
        : 0;
    RzfpAxisLeafRepackStats stats{};
    stats.source_payload_bytes = sourcePayloadBytes;

    const std::array<PlaneAxis, 3> axes{
        PlaneAxis::X,
        PlaneAxis::Y,
        PlaneAxis::Z
    };
    for (size_t i = 0; i < axes.size() && ok; ++i) {
        ok = buildAxisReplica(
            sourceFd,
            outputPath,
            header,
            sbIndex,
            descriptors,
            axes[i],
            descHash,
            memoryLimitMiB,
            stats.replica_bytes[i]);
    }

    if (ok) {
        ok = copyMetadataPrefix(
            sourceFd,
            outputPath,
            header,
            sourcePayloadBytes,
            descHash);
    }
    close(sourceFd);

    if (!ok) {
        unlink(outputPath.c_str());
        for (PlaneAxis axis : axes) {
            const std::string path =
                rzfpAxisLeafPath(outputPath, axis);
            unlink(path.c_str());
            unlink((path + ".tmp").c_str());
        }
        return false;
    }

    stats.metadata_bytes = header.payload_offset;
    stats.total_bytes = stats.metadata_bytes;
    for (uint64_t bytes : stats.replica_bytes) {
        stats.total_bytes += bytes;
    }
    stats.storage_ratio =
        static_cast<double>(stats.total_bytes) /
        static_cast<double>(rzfpRawSize(header));
    if (outputStats) *outputStats = stats;

    std::cout
        << "RZFP axis-leaf repack complete: total_bytes="
        << stats.total_bytes
        << ", storage_ratio="
        << stats.storage_ratio
        << std::endl;
    return true;
}

} // namespace erwt3d
