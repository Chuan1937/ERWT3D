#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace erwt3d {

constexpr uint32_t TAPS_MAGIC = 0x54415053;
constexpr uint32_t TAPS_VERSION = 1;
constexpr uint64_t TAPS_HEADER_SIZE = 512;

enum class TapsCodec : uint8_t {
    Raw = 0,
    LZ4 = 1,
    RZFP2D = 2,
};

struct TapsChunkIndex {
    uint64_t offset;
    uint32_t compressed_size;
    uint32_t raw_size;
    uint32_t plane_index;
    uint8_t codec;
    uint8_t reserved[3];
};

static_assert(sizeof(TapsChunkIndex) == 24, "TapsChunkIndex size");

struct TapsWriteConfig {
    uint64_t nx, ny, nz;
    TapsCodec codec = TapsCodec::LZ4;
    uint64_t chunk_kb = 1024;
    int threads = 8;
    std::string output_dir;
};

struct TapsReadConfig {
    int threads = 8;
    uint64_t memory_limit_mb = 4096;
};

struct TapsSliceRequest {
    char axis;
    uint64_t index;
    float* output;
};

struct TapsStats {
    uint64_t total_raw_bytes = 0;
    uint64_t total_compressed_bytes = 0;
    double storage_ratio = 0;
    double write_seconds = 0;
};

bool tapsWriteFromRaw(const std::string& raw_path, const TapsWriteConfig& config, TapsStats& stats);

class TapsReader {
public:
    explicit TapsReader(const std::string& dir, int threads = 1);
    ~TapsReader();

    TapsReader(const TapsReader&) = delete;
    TapsReader& operator=(const TapsReader&) = delete;

    bool readSlice(char axis, uint64_t index, float* output);
    bool readSlicesBatch(const std::vector<TapsSliceRequest>& requests);

    uint64_t nx() const;
    uint64_t ny() const;
    uint64_t nz() const;
    double storageRatio() const;
    void setThreads(int t);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
