#pragma once

#include <cstdint>
#include <string>

namespace erwt3d {
namespace ssd_cold {

struct ColdProfile {
    uint64_t logical_slice_requests = 0;
    uint64_t logical_leaf_requests = 0;
    uint64_t unique_leaf_records = 0;
    uint64_t duplicate_records_eliminated = 0;

    uint64_t requested_record_bytes = 0;
    uint64_t actual_read_bytes = 0;
    uint64_t main_payload_read_bytes = 0;
    uint64_t axis_x_read_bytes = 0;
    uint64_t axis_y_read_bytes = 0;
    uint64_t axis_z_read_bytes = 0;

    uint64_t pread_calls = 0;
    uint64_t extent_count = 0;
    uint64_t merged_gap_bytes = 0;
    double read_amplification = 1.0;

    double io_time_ms = 0.0;
    double decode_time_ms = 0.0;
    double scatter_time_ms = 0.0;
    double write_time_ms = 0.0;
    double plan_time_ms = 0.0;
    double process_e2e_ms = 0.0;

    uint64_t peak_rss_mib = 0;

    uint64_t decoded_leaf_count = 0;
    uint64_t decoder_error_count = 0;

    std::string physical_device;
    std::string requested_profile;
    std::string resolved_read_strategy;

    bool yz_fallback_to_main = false;
    uint64_t yz_main_fallback_bytes = 0;

    int read_threads = 0;
    int decode_threads = 0;
    int write_threads = 0;
    uint64_t max_gap_bytes = 0;
    uint64_t max_extent_bytes = 0;
};

} // namespace ssd_cold
} // namespace erwt3d
