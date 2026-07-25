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

static ERWT3DHeader planHeaderFromRzfp(const RzfpFileHeader& rh) {
    ERWT3DHeader h{};
    std::memcpy(h.magic, rh.magic, 8);
    h.version = rh.version;
    h.nx = rh.nx; h.ny = rh.ny; h.nz = rh.nz;
    h.dtype = rh.dtype;
    h.super_x = rh.super_x; h.super_y = rh.super_y; h.super_z = rh.super_z;
    h.leaf_x = rh.leaf_x; h.leaf_y = rh.leaf_y; h.leaf_z = rh.leaf_z;
    h.data_offset = rh.data_offset;
    h.flags = rh.flags;
    return h;
}

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

static bool openSectionInfo(
    const std::string& filePath,
    EmbeddedSectionType embeddedType,
    const std::vector<EmbeddedSectionInfo>& embeddedSections,
    const RzfpAxisLeafHeader& alHdr,
    ColdSectionInfo& info
) {
    const EmbeddedSectionInfo* emb = findEmbeddedSection(embeddedSections, embeddedType);
    if (!emb) return false;

    int fd = open(filePath.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;

    info.fd = fd;
    info.base_offset = emb->offset;
    info.section_bytes = emb->bytes;
    info.path = filePath;
    return true;
}

} // anonymous namespace

ColdRequestPlanResult buildRzfpColdRequestPlan(
    const std::string& filePath,
    const ContestPositions& positions,
    const std::vector<ColdSectionInfo>& sectionInfos,
    const RzfpFileHeader& rzfpHdr,
    const std::vector<RzfpLeafDescriptor>& descriptors)
{
    ColdRequestPlanResult result;
    result.nx = rzfpHdr.nx;
    result.ny = rzfpHdr.ny;
    result.nz = rzfpHdr.nz;

    ColdRequestPlan& plan = result.plan;
    plan.format = ColdFormat::RZFP;

    struct GroupEntry {
        SliceAxis axis;
        std::string name;
        const std::vector<uint64_t>* indices = nullptr;
        uint32_t group_id = 0;
    };

    std::vector<GroupEntry> groups;
    uint32_t gid = 0;
    groups.push_back({SliceAxis::X, "x_random", &positions.x_random, gid++});
    groups.push_back({SliceAxis::Y, "y_random", &positions.y_random, gid++});
    groups.push_back({SliceAxis::Z, "z_random", &positions.z_random, gid++});
    groups.push_back({SliceAxis::X, "x_continuous", &positions.x_continuous, gid++});
    groups.push_back({SliceAxis::Y, "y_continuous", &positions.y_continuous, gid++});
    groups.push_back({SliceAxis::Z, "z_continuous", &positions.z_continuous, gid++});

    const auto axisToSection = [&](SliceAxis ax) -> ColdRecordSource {
        switch (ax) {
            case SliceAxis::X: return ColdRecordSource::RzfpAxisLeafX;
            case SliceAxis::Y: return ColdRecordSource::RzfpAxisLeafY;
            case SliceAxis::Z: return ColdRecordSource::RzfpAxisLeafZ;
            default: return ColdRecordSource::UnknownSource;
        }
    };

    const int sectionIdxFor[3] = {
        static_cast<int>(ColdRecordSource::RzfpAxisLeafX) -
        static_cast<int>(ColdRecordSource::RzfpAxisLeafX),
        static_cast<int>(ColdRecordSource::RzfpAxisLeafY) -
        static_cast<int>(ColdRecordSource::RzfpAxisLeafX),
        static_cast<int>(ColdRecordSource::RzfpAxisLeafZ) -
        static_cast<int>(ColdRecordSource::RzfpAxisLeafX),
    };

    bool hasAllSections = true;
    std::array<ColdSectionInfo, 3> secInfo;
    for (int i = 0; i < 3; ++i) {
        int fi = -1;
        for (size_t si = 0; si < sectionInfos.size(); ++si) {
            if (sectionInfos[si].source == static_cast<ColdRecordSource>(
                    static_cast<int>(ColdRecordSource::RzfpAxisLeafX) + i)) {
                fi = static_cast<int>(si);
                break;
            }
        }
        if (fi < 0 || sectionInfos[fi].fd < 0) {
            hasAllSections = false;
            break;
        }
        secInfo[i] = sectionInfos[fi];
    }

    if (!hasAllSections) {
        result.error = "missing RZFP axis-leaf embedded sections (need X, Y, Z)";
        return result;
    }

    const uint64_t leavesPerSB = rzfpTotalLeafsPerSuper(rzfpHdr);
    const uint64_t totalLeaves = rzfpSuperGridX(rzfpHdr) *
                                  rzfpSuperGridY(rzfpHdr) *
                                  rzfpSuperGridZ(rzfpHdr) * leavesPerSB;

    std::array<std::vector<RzfpAxisLeafSlabIndex>, 3> slabIndexes;
    std::array<RzfpAxisLeafHeader, 3> alHeaders;
    for (int ai = 0; ai < 3; ++ai) {
        const auto& info = secInfo[ai];
        const uint64_t hdrOffset = info.base_offset;
        ssize_t nr = pread(info.fd, &alHeaders[ai], sizeof(RzfpAxisLeafHeader),
                           static_cast<off_t>(hdrOffset));
        if (nr != sizeof(RzfpAxisLeafHeader)) {
            result.error = "failed to read axis-leaf header for axis " + std::to_string(ai);
            return result;
        }

        const uint64_t indexBytes = alHeaders[ai].slab_count * sizeof(RzfpAxisLeafSlabIndex);
        slabIndexes[ai].resize(alHeaders[ai].slab_count);
        nr = pread(info.fd, slabIndexes[ai].data(), indexBytes,
                   static_cast<off_t>(hdrOffset + alHeaders[ai].index_offset));
        if (nr != static_cast<ssize_t>(indexBytes)) {
            result.error = "failed to read axis-leaf index for axis " + std::to_string(ai);
            return result;
        }
    }

    const uint64_t leafX = rzfpHdr.leaf_x;
    const uint64_t leafY = rzfpHdr.leaf_y;
    const uint64_t leafZ = rzfpHdr.leaf_z;

    struct LogicalLeafKey {
        uint64_t slab_id;
        uint64_t descriptor_id;
        ColdRecordSource source;

        bool operator==(const LogicalLeafKey& o) const {
            return slab_id == o.slab_id && descriptor_id == o.descriptor_id && source == o.source;
        }
    };
    struct KeyHash {
        size_t operator()(const LogicalLeafKey& k) const {
            return static_cast<size_t>(k.slab_id ^
                   (k.descriptor_id * 0x9e3779b97f4a7c15ULL) ^
                   (static_cast<uint64_t>(k.source) << 56));
        }
    };

    std::unordered_map<LogicalLeafKey, size_t, KeyHash> recordMap;

    for (const auto& group : groups) {
        const auto axis = group.axis;
        const int ai = static_cast<int>(axis);
        const uint64_t axisSize = (axis == SliceAxis::X) ? rzfpHdr.nx
                                : (axis == SliceAxis::Y) ? rzfpHdr.ny
                                : rzfpHdr.nz;
        const uint64_t leSize = (axis == SliceAxis::X) ? leafX
                              : (axis == SliceAxis::Y) ? leafY
                              : leafZ;
        const auto sectionSrc = axisToSection(axis);

        for (size_t si = 0; si < group.indices->size(); ++si) {
            const uint64_t sliceIdx = (*group.indices)[si];
            if (sliceIdx >= axisSize) {
                result.error = "slice index " + std::to_string(sliceIdx) + " out of range";
                return result;
            }

            ColdSliceRequest sreq;
            sreq.axis = axis;
            sreq.index = sliceIdx;
            sreq.group_name = group.name;
            plan.slice_requests.push_back(sreq);
            plan.logical_slice_requests++;

            const uint64_t slab = sliceIdx / leSize;
            if (slab >= slabIndexes[ai].size()) {
                result.error = "slab " + std::to_string(slab) + " out of range";
                return result;
            }

            const auto& slabIdx = slabIndexes[ai][slab];
            const uint64_t slabFileOffset = secInfo[ai].base_offset +
                alHeaders[ai].payload_offset + slabIdx.offset;
            const uint64_t slabSize = slabIdx.bytes;

            uint64_t recOffset = 0;
            while (recOffset < slabSize) {
                if (slabSize - recOffset < sizeof(uint32_t)) break;
                uint32_t descriptorId = 0;
                const uint64_t descFileOff = slabFileOffset + recOffset;
                std::memcpy(&descriptorId, &descriptorId, 0);
                {
                    uint8_t buf[4];
                    ssize_t br = pread(secInfo[ai].fd, buf, 4,
                                       static_cast<off_t>(descFileOff));
                    if (br != 4) break;
                    std::memcpy(&descriptorId, buf, 4);
                }
                recOffset += sizeof(uint32_t);

                if (descriptorId >= descriptors.size()) break;
                const auto desc = descriptors[descriptorId];
                const uint16_t recSize = descriptorSize(desc);
                if (recSize > slabSize - recOffset) break;

                uint64_t gx = 0, gy = 0, gz = 0;
                axisLeafRecordCoordinates(rzfpHdr, descriptorId, gx, gy, gz);

                const uint64_t leafXSz = std::min<uint64_t>(leafX, rzfpHdr.nx - gx);
                const uint64_t leafYSz = std::min<uint64_t>(leafY, rzfpHdr.ny - gy);
                const uint64_t leafZSz = std::min<uint64_t>(leafZ, rzfpHdr.nz - gz);

                uint32_t local = static_cast<uint32_t>(sliceIdx % leSize);
                bool relevant = false;
                switch (axis) {
                    case SliceAxis::X:
                        relevant = (gx / leSize == slab) && (local < leafXSz);
                        break;
                    case SliceAxis::Y:
                        relevant = (gy / leSize == slab) && (local < leafYSz);
                        break;
                    case SliceAxis::Z:
                        relevant = (gz / leSize == slab) && (local < leafZSz);
                        break;
                }
                if (!relevant) { recOffset += recSize; continue; }

                LogicalLeafKey key{slab, descriptorId, sectionSrc};
                auto it = recordMap.find(key);
                if (it == recordMap.end()) {
                    const uint64_t recFileOff = descFileOff + sizeof(uint32_t);

                    ColdLeafRecord rec;
                    rec.file_offset = recFileOff;
                    rec.record_size = recSize;
                    rec.source = sectionSrc;
                    rec.descriptor_id = descriptorId;
                    rec.codec = descriptorCodec(desc);
                    rec.gx = gx; rec.gy = gy; rec.gz = gz;
                    rec.outputs.push_back({group.group_id, static_cast<uint32_t>(si)});
                    rec.visited = true;

                    recordMap[key] = plan.records.size();
                    plan.records.push_back(std::move(rec));
                    plan.unique_leaf_records++;
                    plan.requested_record_bytes += recSize;
                    plan.logical_leaf_requests++;
                } else {
                    auto& existing = plan.records[it->second];
                    existing.outputs.push_back({group.group_id, static_cast<uint32_t>(si)});
                    plan.duplicate_records_eliminated++;
                    plan.logical_leaf_requests++;
                }

                recOffset += recSize;
            }

            plan.logical_leaf_requests = plan.unique_leaf_records + plan.duplicate_records_eliminated;
        }
    }

    for (auto& rec : plan.records) {
        switch (rec.source) {
            case ColdRecordSource::RzfpAxisLeafX: plan.axis_x_read_bytes += rec.record_size; break;
            case ColdRecordSource::RzfpAxisLeafY: plan.axis_y_read_bytes += rec.record_size; break;
            case ColdRecordSource::RzfpAxisLeafZ: plan.axis_z_read_bytes += rec.record_size; break;
            default: break;
        }
    }

    plan.main_payload_read_bytes = 0;
    plan.all_records_routed_to_sections = true;

    result.ok = true;
    return result;
}

ColdRequestPlanResult buildLz4ColdRequestPlan(
    const std::string& filePath,
    const ContestPositions& positions,
    const std::vector<ColdSectionInfo>& sectionInfos,
    const ERWT3DHeader& header)
{
    ColdRequestPlanResult result;
    result.nx = header.nx;
    result.ny = header.ny;
    result.nz = header.nz;

    (void)filePath;
    (void)positions;
    (void)sectionInfos;
    (void)header;

    result.ok = true;
    return result;
}

ColdRequestPlanResult buildColdRequestPlan(
    const std::string& filePath,
    const ContestPositions& positions)
{
    ColdRequestPlanResult result;

    {
        int testFd = open(filePath.c_str(), O_RDONLY | O_CLOEXEC);
        if (testFd < 0) {
            result.error = "cannot open file: " + filePath;
            return result;
        }
        uint8_t magic[8] = {};
        if (pread(testFd, magic, 8, 0) != 8) {
            close(testFd);
            result.error = "cannot read file magic";
            return result;
        }
        close(testFd);

        if (std::memcmp(magic, "ERWT3DR\0", 8) == 0) {
            RzfpFileHeader rzfpHdr{};
            int fd = open(filePath.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) { result.error = "cannot open RZFP file"; return result; }
            if (pread(fd, &rzfpHdr, sizeof(RzfpFileHeader), 0) != sizeof(RzfpFileHeader)) {
                close(fd); result.error = "cannot read RZFP header"; return result;
            }

            std::vector<RzfpLeafDescriptor> descriptors;
            {
                uint64_t payloadBytes = 0;
                {
                    struct stat st{};
                    if (fstat(fd, &st) != 0) { close(fd); result.error = "stat failed"; return result; }
                    const uint64_t dataEnd = rzfpHdr.data_offset > 0 ? rzfpHdr.data_offset : sizeof(RzfpFileHeader);
                    payloadBytes = st.st_size - dataEnd;
                }

                const uint64_t totalLeaves = rzfpSuperGridX(rzfpHdr) * rzfpSuperGridY(rzfpHdr) *
                                              rzfpSuperGridZ(rzfpHdr) * rzfpTotalLeafsPerSuper(rzfpHdr);
                const uint64_t descOffset = rzfpHdr.descriptor_offset;
                const uint64_t descBytes = totalLeaves * sizeof(RzfpLeafDescriptor);
                descriptors.resize(totalLeaves);
                ssize_t nr = pread(fd, descriptors.data(), descBytes,
                                    static_cast<off_t>(descOffset));
                if (nr != static_cast<ssize_t>(descBytes)) {
                    close(fd); result.error = "cannot read descriptors"; return result;
                }
            }

            std::vector<EmbeddedSectionInfo> embSections;
            if (hasEmbeddedSections(rzfpHdr)) {
                if (!readEmbeddedSectionDirectory(fd,
                        getEmbeddedSectionDirectoryOffset(rzfpHdr),
                        getEmbeddedSectionDirectoryBytes(rzfpHdr),
                        0, embSections)) {
                    close(fd); result.error = "cannot read embedded section directory"; return result;
                }
            }

            std::vector<ColdSectionInfo> sectionInfos;
            for (int ai = 0; ai < 3; ++ai) {
                const auto embType = static_cast<EmbeddedSectionType>(
                    static_cast<uint32_t>(EmbeddedSectionType::RzfpAxisLeafX) + ai);
                const EmbeddedSectionInfo* emb = findEmbeddedSection(embSections, embType);
                if (emb) {
                    int secFd = open(filePath.c_str(), O_RDONLY | O_CLOEXEC);
                    if (secFd >= 0) {
                        ColdSectionInfo info;
                        info.source = static_cast<ColdRecordSource>(
                            static_cast<int>(ColdRecordSource::RzfpAxisLeafX) + ai);
                        info.fd = secFd;
                        info.base_offset = emb->offset;
                        info.section_bytes = emb->bytes;
                        info.path = filePath;
                        sectionInfos.push_back(std::move(info));
                    }
                }
            }

            close(fd);

            return buildRzfpColdRequestPlan(filePath, positions, sectionInfos, rzfpHdr, descriptors);
        } else if (std::memcmp(magic, "ERWT3D\0", 7) == 0) {
            ERWT3DHeader hdr{};
            int fd = open(filePath.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) { result.error = "cannot open LZ4 file"; return result; }
            if (pread(fd, &hdr, sizeof(ERWT3DHeader), 0) != sizeof(ERWT3DHeader)) {
                close(fd); result.error = "cannot read LZ4 header"; return result;
            }
            close(fd);

            std::vector<ColdSectionInfo> sectionInfos;
            result = buildLz4ColdRequestPlan(filePath, positions, sectionInfos, hdr);
            return result;
        } else {
            result.error = "unknown file format";
            return result;
        }
    }
}

} // namespace ssd_cold
} // namespace erwt3d
