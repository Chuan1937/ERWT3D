#pragma once

#include "rzfp_codec.hpp"
#include "rzfp_format.hpp"
#include "format.hpp"
#include "raw_x_aux.hpp"

#include <cstdint>
#include <string>

namespace erwt3d {

struct RzfpWriterConfig {
    uint64_t nx = 0;
    uint64_t ny = 0;
    uint64_t nz = 0;

    uint32_t super_size = 64;
    uint32_t leaf_size = 4;

    size_t memory_limit_mb = 2048;
    int threads = 8;

    RzfpCodecConfig codec;

    PhysicalOrder physical_order = PhysicalOrder::ZYX;
};

struct RzfpWriterStats {
    uint64_t total_leaves = 0;
    uint64_t raw_leaves = 0;
    uint64_t zero_leaves = 0;
    uint64_t constant_leaves = 0;
    uint64_t accuracy_leaves = 0;
    uint64_t accuracy_exception_leaves = 0;
    uint64_t precision_leaves = 0;

    uint64_t total_exceptions = 0;
    double average_exceptions_per_leaf = 0.0;
    uint32_t max_exceptions = 0;

    uint64_t payload_bytes = 0;
    uint64_t descriptor_bytes = 0;
    uint64_t index_bytes = 0;

    double storage_ratio = 0.0;
    double max_relative_error = 0.0;
    uint64_t violation_count = 0;

    double encode_seconds = 0.0;
    double io_seconds = 0.0;
};

bool writeRzfpFile(
    const std::string& raw_path,
    const std::string& output_path,
    const RzfpWriterConfig& config,
    RzfpWriterStats* stats = nullptr
);

bool appendRawXAuxToRzfpFile(
    const std::string& rzfpPath,
    const std::string& rawPath,
    uint64_t nx, uint64_t ny, uint64_t nz,
    RawXAuxStats* stats = nullptr,
    bool forceEdge = false
);

} // namespace erwt3d
