#pragma once

#include "erwt3d/format.hpp"
#include "erwt3d/rzfp_format.hpp"
#include "erwt3d/rzfp_axis_leaf.hpp"
#include "erwt3d/axis_plane.hpp"
#include "erwt3d/embedded_sections.hpp"
#include "erwt3d/contest_positions.hpp"
#include "erwt3d/slice.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace erwt3d {
namespace ssd_cold {

enum class ColdFormat { Unknown, LZ4_ERWT3D, RZFP };

enum class ColdRecordSource { RzfpAxisLeafX, RzfpAxisLeafY, RzfpAxisLeafZ,
                              RzfpMainPayload,
                              Lz4WholePlaneY, Lz4WholePlaneZ,
                              Lz4Superblock, Lz4SuperblockCompressed,
                              UnknownSource };

struct ColdOutputTarget {
    uint32_t output_slot = 0;
    uint8_t local = 0;
};

struct ColdSlabRequest {
    ColdRecordSource source = ColdRecordSource::UnknownSource;
    int fd = -1;
    uint64_t file_offset = 0;
    uint64_t slab_bytes = 0;
    uint64_t slab_id = 0;
    std::vector<ColdOutputTarget> targets;
};

struct ColdSliceRequest {
    SliceAxis axis = SliceAxis::X;
    uint64_t index = 0;
    std::string group_name;
};

struct ColdRequestPlan {
    ColdFormat format = ColdFormat::Unknown;

    std::vector<ColdSliceRequest> slice_requests;
    std::vector<ColdSlabRequest> slab_requests;

    uint64_t logical_slice_requests = 0;
    uint64_t unique_slabs = 0;
    uint64_t duplicate_slabs_eliminated = 0;

    uint64_t requested_record_bytes = 0;
    uint64_t main_payload_read_bytes = 0;
    uint64_t axis_x_read_bytes = 0;
    uint64_t axis_y_read_bytes = 0;
    uint64_t axis_z_read_bytes = 0;

    bool all_records_routed_to_sections = false;
};

struct ColdRequestPlanResult {
    bool ok = false;
    ColdRequestPlan plan;
    std::string error;

    uint64_t nx = 0;
    uint64_t ny = 0;
    uint64_t nz = 0;
    uint64_t leaf_x = 4;
    uint64_t leaf_y = 4;
    uint64_t leaf_z = 4;

    std::string file_path;
    int main_fd = -1;

    RzfpFileHeader rzfp_header{};
    std::vector<RzfpLeafDescriptor> descriptors;
    std::vector<int> section_fds;
    std::vector<ColdRecordSource> section_sources;
    bool is_rzfp = false;
};

ColdRequestPlanResult buildColdRequestPlan(
    const std::string& filePath,
    const ContestPositions& positions);

} // namespace ssd_cold
} // namespace erwt3d
