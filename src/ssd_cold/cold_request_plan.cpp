#include "erwt3d/ssd_cold/cold_request_plan.hpp"
#include "erwt3d/rzfp_codec.hpp"
#include "erwt3d/morton.hpp"
#include "erwt3d/sb_plan.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unordered_map>
#include <unistd.h>

namespace erwt3d {
namespace ssd_cold {

namespace {

static uint64_t axisLeafDescriptorHash(
    const std::vector<RzfpLeafDescriptor>& descriptors
) {
    uint64_t hash = 1469598103934665603ULL;
    const auto* data = reinterpret_cast<const uint8_t*>(descriptors.data());
    const uint64_t bytes = descriptors.size() * sizeof(RzfpLeafDescriptor);
    for (uint64_t i = 0; i < bytes; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static void axisLeafRecordCoordinates(
    const RzfpFileHeader& header,
    uint64_t descriptorId,
    uint64_t& gx, uint64_t& gy, uint64_t& gz
) {
    const uint64_t leavesPerSB = rzfpTotalLeafsPerSuper(header);
    const uint64_t physicalSb = descriptorId / leavesPerSB;
    const uint32_t morton = static_cast<uint32_t>(descriptorId % leavesPerSB);

    const uint64_t sgX = rzfpSuperGridX(header);
    const uint64_t sgY = rzfpSuperGridY(header);
    const uint64_t sgZ = rzfpSuperGridZ(header);
    const uint64_t sx = physicalSb % sgX;
    const uint64_t rem = physicalSb / sgX;
    uint64_t sy = 0, sz = 0;
    if ((header.flags & FLAG_PHYSICAL_ORDER_YZX) != 0) {
        sz = rem % sgZ;
        sy = rem / sgZ;
    } else {
        sy = rem % sgY;
        sz = rem / sgY;
    }

    uint32_t lx = 0, ly = 0, lz = 0;
    unmorton3D(morton, lx, ly, lz);
    gx = sx * header.super_x + static_cast<uint64_t>(lx) * header.leaf_x;
    gy = sy * header.super_y + static_cast<uint64_t>(ly) * header.leaf_y;
    gz = sz * header.super_z + static_cast<uint64_t>(lz) * header.leaf_z;
}

static uint16_t descriptorSize(RzfpLeafDescriptor d) {
    return static_cast<uint16_t>(d & 0x1FFFu);
}

static RzfpLeafCodec descriptorCodec(RzfpLeafDescriptor d) {
    return static_cast<RzfpLeafCodec>((d >> 13) & 0x7u);
}

} // anonymous namespace

ColdRequestPlanResult buildColdRequestPlan(
    const std::string& filePath,
    const ContestPositions& positions)
{
    ColdRequestPlanResult result;
    result.file_path = filePath;

    uint8_t magic[8] = {};
    {
        int testFd = open(filePath.c_str(), O_RDONLY | O_CLOEXEC);
        if (testFd < 0) {
            result.error = "cannot open file: " + filePath;
            return result;
        }
        if (pread(testFd, magic, 8, 0) != 8) {
            close(testFd);
            result.error = "cannot read file magic";
            return result;
        }
        close(testFd);
    }

    std::array<std::vector<RzfpAxisLeafSlabIndex>, 3> slabIndexes;
    std::array<RzfpAxisLeafHeader, 3> alHeaders{};
    std::array<uint64_t, 3> payloadBases{};

    if (std::memcmp(magic, "ERWT3DR\0", 8) == 0) {
        result.is_rzfp = true;

        int fd = open(filePath.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) { result.error = "cannot open RZFP file"; return result; }
        result.main_fd = fd;

        RzfpFileHeader& rzfpHdr = result.rzfp_header;
        if (pread(fd, &rzfpHdr, sizeof(RzfpFileHeader), 0) != static_cast<ssize_t>(sizeof(RzfpFileHeader))) {
            result.error = "cannot read RZFP header"; result.main_fd = -1; return result;
        }

        result.nx = rzfpHdr.nx;
        result.ny = rzfpHdr.ny;
        result.nz = rzfpHdr.nz;
        result.leaf_x = rzfpHdr.leaf_x;
        result.leaf_y = rzfpHdr.leaf_y;
        result.leaf_z = rzfpHdr.leaf_z;

        const uint64_t totalLeaves = rzfpSuperGridX(rzfpHdr) * rzfpSuperGridY(rzfpHdr) *
                                      rzfpSuperGridZ(rzfpHdr) * rzfpTotalLeafsPerSuper(rzfpHdr);
        result.descriptors.resize(totalLeaves);
        ssize_t nr = pread(fd, result.descriptors.data(),
                            totalLeaves * sizeof(RzfpLeafDescriptor),
                            static_cast<off_t>(rzfpHdr.descriptor_offset));
        if (nr != static_cast<ssize_t>(totalLeaves * sizeof(RzfpLeafDescriptor))) {
            result.error = "cannot read descriptors"; result.main_fd = -1; return result;
        }

        std::vector<EmbeddedSectionInfo> embSections;
        if (hasEmbeddedSections(rzfpHdr)) {
            struct stat fileStat{};
            if (fstat(fd, &fileStat) != 0) {
                result.error = "cannot stat file"; result.main_fd = -1; return result;
            }
            if (!readEmbeddedSectionDirectory(fd,
                    getEmbeddedSectionDirectoryOffset(rzfpHdr),
                    getEmbeddedSectionDirectoryBytes(rzfpHdr),
                    static_cast<uint64_t>(fileStat.st_size),
                    embSections)) {
                result.error = "cannot read embedded sections"; result.main_fd = -1; return result;
            }
        }

        std::array<EmbeddedSectionInfo*, 3> embSec{nullptr, nullptr, nullptr};
        for (int ai = 0; ai < 3; ++ai) {
            const auto embType = static_cast<EmbeddedSectionType>(
                static_cast<uint32_t>(EmbeddedSectionType::RzfpAxisLeafX) + ai);
            embSec[ai] = const_cast<EmbeddedSectionInfo*>(
                findEmbeddedSection(embSections, embType));
        }

        bool hasAll = embSec[0] && embSec[1] && embSec[2];
        if (!hasAll) {
            result.error = "missing RZFP axis-leaf sections (X/Y/Z required)"; result.main_fd = -1; return result;
        }

        for (int ai = 0; ai < 3; ++ai) {
            int secFd = open(filePath.c_str(), O_RDONLY | O_CLOEXEC);
            if (secFd < 0) { result.error = "cannot open section fd"; result.main_fd = -1; return result; }
            result.section_fds.push_back(secFd);
            result.section_sources.push_back(
                static_cast<ColdRecordSource>(
                    static_cast<int>(ColdRecordSource::RzfpAxisLeafX) + ai));
        }

        std::array<EmbeddedSectionInfo*, 3> embSecPtr{embSec[0], embSec[1], embSec[2]};
        for (int ai = 0; ai < 3; ++ai) {
            const uint64_t embOffset = embSecPtr[ai]->offset;
            if (pread(result.section_fds[ai], &alHeaders[ai], sizeof(RzfpAxisLeafHeader),
                      static_cast<off_t>(embOffset)) != static_cast<ssize_t>(sizeof(RzfpAxisLeafHeader))) {
                result.error = "cannot read axis-leaf header for axis " + std::to_string(ai);
                return result;
            }

            const uint64_t idxSize = alHeaders[ai].slab_count * sizeof(RzfpAxisLeafSlabIndex);
            slabIndexes[ai].resize(alHeaders[ai].slab_count);
            if (pread(result.section_fds[ai], slabIndexes[ai].data(), idxSize,
                      static_cast<off_t>(embOffset + alHeaders[ai].index_offset)) !=
                static_cast<ssize_t>(idxSize)) {
                result.error = "cannot read axis-leaf index for axis " + std::to_string(ai);
                return result;
            }

            payloadBases[ai] = embOffset + alHeaders[ai].payload_offset;
        }
    } else if (std::memcmp(magic, "ERWT3D\0", 7) == 0) {
        result.is_rzfp = false;
        int fd = open(filePath.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) { result.error = "cannot open LZ4 file"; return result; }
        result.main_fd = fd;

        ERWT3DHeader hdr{};
        if (pread(fd, &hdr, sizeof(ERWT3DHeader), 0) != static_cast<ssize_t>(sizeof(ERWT3DHeader))) {
            result.error = "cannot read LZ4 header"; result.main_fd = -1; return result;
        }
        result.nx = hdr.nx;
        result.ny = hdr.ny;
        result.nz = hdr.nz;
        result.leaf_x = hdr.leaf_x;
        result.leaf_y = hdr.leaf_y;
        result.leaf_z = hdr.leaf_z;
    } else {
        result.error = "unknown file format";
        return result;
    }

    if (!result.is_rzfp) {
        result.ok = true;
        result.plan.format = ColdFormat::LZ4_ERWT3D;
        return result;
    }

    ColdRequestPlan& plan = result.plan;
    plan.format = ColdFormat::RZFP;

    const auto& rzfpHdr = result.rzfp_header;
    const uint64_t leafX = rzfpHdr.leaf_x;
    const uint64_t leafY = rzfpHdr.leaf_y;
    const uint64_t leafZ = rzfpHdr.leaf_z;

    struct GroupEntry {
        SliceAxis axis;
        std::string name;
        const std::vector<uint64_t>* indices;
        uint32_t group_id;
    };

    std::vector<GroupEntry> groups;
    {
        uint32_t gid = 0;
        groups.push_back({SliceAxis::X, "x_random", &positions.x_random, gid++});
        groups.push_back({SliceAxis::Y, "y_random", &positions.y_random, gid++});
        groups.push_back({SliceAxis::Z, "z_random", &positions.z_random, gid++});
        groups.push_back({SliceAxis::X, "x_continuous", &positions.x_continuous, gid++});
        groups.push_back({SliceAxis::Y, "y_continuous", &positions.y_continuous, gid++});
        groups.push_back({SliceAxis::Z, "z_continuous", &positions.z_continuous, gid++});
    }

    const auto axisToSrc = [](SliceAxis ax) -> ColdRecordSource {
        switch (ax) {
            case SliceAxis::X: return ColdRecordSource::RzfpAxisLeafX;
            case SliceAxis::Y: return ColdRecordSource::RzfpAxisLeafY;
            case SliceAxis::Z: return ColdRecordSource::RzfpAxisLeafZ;
            default: return ColdRecordSource::UnknownSource;
        }
    };

    struct SlabKey {
        ColdRecordSource source;
        uint64_t slab_id;
        bool operator==(const SlabKey& o) const {
            return source == o.source && slab_id == o.slab_id;
        }
    };
    struct SlabKeyHash {
        size_t operator()(const SlabKey& k) const {
            return static_cast<size_t>(static_cast<int>(k.source) * 31 + k.slab_id);
        }
    };

    uint32_t outputSlot = 0;
    std::unordered_map<SlabKey, size_t, SlabKeyHash> slabMap;

    for (const auto& group : groups) {
        const auto axis = group.axis;
        const int ai = static_cast<int>(axis);
        const uint64_t axisSize = (axis == SliceAxis::X) ? rzfpHdr.nx
                                : (axis == SliceAxis::Y) ? rzfpHdr.ny
                                : rzfpHdr.nz;
        const uint64_t leSize = (axis == SliceAxis::X) ? leafX
                              : (axis == SliceAxis::Y) ? leafY
                              : leafZ;
        const auto src = axisToSrc(axis);

        for (size_t si = 0; si < group.indices->size(); ++si) {
            const uint64_t sliceIdx = (*group.indices)[si];
            if (sliceIdx >= axisSize) {
                result.error = "slice index out of range";
                return result;
            }

            ColdSliceRequest sreq;
            sreq.axis = axis; sreq.index = sliceIdx; sreq.group_name = group.name;
            plan.slice_requests.push_back(sreq);
            plan.logical_slice_requests++;

            const uint64_t slab = sliceIdx / leSize;
            const uint32_t local = static_cast<uint32_t>(sliceIdx % leSize);

            if (slab >= slabIndexes[ai].size()) {
                result.error = "slab out of range";
                return result;
            }

            SlabKey key{src, slab};
            auto it = slabMap.find(key);
            if (it == slabMap.end()) {
                ColdSlabRequest slabReq;
                slabReq.source = src;
                slabReq.fd = result.section_fds[ai];
                slabReq.file_offset = payloadBases[ai] + slabIndexes[ai][slab].offset;
                slabReq.slab_bytes = slabIndexes[ai][slab].bytes;
                slabReq.slab_id = slab;
                slabReq.targets.push_back({outputSlot, static_cast<uint8_t>(local)});

                slabMap[key] = plan.slab_requests.size();
                plan.slab_requests.push_back(std::move(slabReq));
                plan.unique_slabs++;
            } else {
                auto& existing = plan.slab_requests[slabMap[key]];
                existing.targets.push_back({outputSlot, static_cast<uint8_t>(local)});
                plan.duplicate_slabs_eliminated++;
            }

            ++outputSlot;
        }
    }

    for (auto& slab : plan.slab_requests) {
        plan.requested_record_bytes += slab.slab_bytes;
        switch (slab.source) {
            case ColdRecordSource::RzfpAxisLeafX: plan.axis_x_read_bytes += slab.slab_bytes; break;
            case ColdRecordSource::RzfpAxisLeafY: plan.axis_y_read_bytes += slab.slab_bytes; break;
            case ColdRecordSource::RzfpAxisLeafZ: plan.axis_z_read_bytes += slab.slab_bytes; break;
            default: break;
        }
    }

    std::sort(plan.slab_requests.begin(), plan.slab_requests.end(),
        [](const ColdSlabRequest& a, const ColdSlabRequest& b) {
            return a.file_offset < b.file_offset;
        });

    plan.main_payload_read_bytes = 0;
    plan.all_records_routed_to_sections = true;

    result.ok = true;
    return result;
}

} // namespace ssd_cold
} // namespace erwt3d
