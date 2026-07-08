#include "erwt3d/thread_pool.hpp"
#include <zfp.h>
#include <zfp/bitstream.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <thread>
#include <atomic>

struct ScanOptions {
    std::string rawPath;
    std::string outPath;
    uint64_t nx = 0, ny = 0, nz = 0;
    std::vector<double> rates;
    double relTol = 1e-3;
    double zeroAbsTol = 1e-6;
    int threads = 0;
    int slabZLayers = 64;
};

struct RateStats {
    double rate = 0;
    uint64_t totalBlocks = 0;
    uint64_t exceptionBlocks = 0;
    uint64_t nearZeroExceptionBlocks = 0;
    uint64_t relErrExceptionBlocks = 0;
    double maxRelError = 0.0;
    double sumCompressedBits = 0.0;
    std::vector<double> blockMaxRelErrors;
};

struct BlockResult {
    std::vector<double> maxRelErrors;
    std::vector<bool> isException;
    std::vector<bool> hasNearZeroFailure;
    std::vector<bool> hasRelErrFailure;
    std::vector<double> compressedBits;
};

static void printUsage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " --raw PATH --nx N --ny N --nz N [options]\n"
        << "  --rates R1,R2,...   ZFP fixed rates in bits/value (default: 4,8,12,16,20,24)\n"
        << "  --rel-tol V         Relative error threshold (default: 1e-3)\n"
        << "  --zero-abs-tol V    Near-zero absolute threshold (default: 1e-6)\n"
        << "  --threads N         Thread count (default: hardware_concurrency)\n"
        << "  --slab-z-layers N   Z layers per slab (default: 64)\n"
        << "  --out PATH          JSON output file (default: stdout)\n";
}

static std::vector<double> parseRates(const std::string& s) {
    std::vector<double> rates;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (!tok.empty())
            rates.push_back(std::stod(tok));
    }
    return rates;
}

static bool parseArgs(int argc, char* argv[], ScanOptions& opt) {
    auto next = [&](int& i) -> std::string {
        if (i + 1 >= argc) return "";
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--raw") opt.rawPath = next(i);
        else if (arg == "--nx") opt.nx = std::stoull(next(i));
        else if (arg == "--ny") opt.ny = std::stoull(next(i));
        else if (arg == "--nz") opt.nz = std::stoull(next(i));
        else if (arg == "--rates") opt.rates = parseRates(next(i));
        else if (arg == "--rel-tol") opt.relTol = std::stod(next(i));
        else if (arg == "--zero-abs-tol") opt.zeroAbsTol = std::stod(next(i));
        else if (arg == "--threads") opt.threads = std::stoi(next(i));
        else if (arg == "--slab-z-layers") opt.slabZLayers = std::stoi(next(i));
        else if (arg == "--out") opt.outPath = next(i);
        else if (arg == "--help" || arg == "-h") { printUsage(argv[0]); return false; }
        else { std::cerr << "Unknown arg: " << arg << "\n"; return false; }
    }
    if (opt.rawPath.empty() || opt.nx == 0 || opt.ny == 0 || opt.nz == 0) {
        std::cerr << "Error: --raw, --nx, --ny, --nz are required\n";
        return false;
    }
    if (opt.rates.empty()) opt.rates = {4, 8, 12, 16, 20, 24};
    if (opt.threads <= 0) opt.threads = (int)std::thread::hardware_concurrency();
    if (opt.threads <= 0) opt.threads = 1;
    return true;
}

static inline bool isFailed(float ref, float actual, double relTol, double zeroAbsTol,
                            double& relErr, bool& isNearZeroFailure) {
    double absErr = std::abs((double)ref - (double)actual);
    double absRef = std::abs((double)ref);
    relErr = absErr / std::max(absRef, 1e-12);
    isNearZeroFailure = false;
    if (absRef <= zeroAbsTol) {
        isNearZeroFailure = true;
        return absErr > zeroAbsTol;
    }
    return relErr >= relTol;
}

static constexpr size_t BLOCK_DIM = 4;
static constexpr size_t BLOCK_FLOATS = 64;
static constexpr size_t BLOCK_BYTES = 256;

struct ZfpThreadState {
    zfp_stream* zs = nullptr;
    zfp_field* field = nullptr;
    zfp_field* fieldDec = nullptr;
    bitstream* bs = nullptr;
    float blockOrig[BLOCK_FLOATS];
    float blockDec[BLOCK_FLOATS];
    std::vector<uint8_t> compBuf;

    void init(size_t compBufSize) {
        compBuf.resize(compBufSize, 0);
        std::memset(blockOrig, 0, BLOCK_BYTES);
        std::memset(blockDec, 0, BLOCK_BYTES);
        field = zfp_field_3d(blockOrig, zfp_type_float, BLOCK_DIM, BLOCK_DIM, BLOCK_DIM);
        fieldDec = zfp_field_3d(blockDec, zfp_type_float, BLOCK_DIM, BLOCK_DIM, BLOCK_DIM);
        zs = zfp_stream_open(nullptr);
        bs = stream_open(compBuf.data(), compBufSize);
        zfp_stream_set_bit_stream(zs, bs);
    }

    ~ZfpThreadState() {
        if (field) zfp_field_free(field);
        if (fieldDec) zfp_field_free(fieldDec);
        if (zs) zfp_stream_close(zs);
        if (bs) stream_close(bs);
    }

    // Returns compressed bits, or 0 on failure
    size_t compressAtRate(double rate) {
        zfp_stream_set_rate(zs, rate, zfp_type_float, 3, zfp_false);
        zfp_field_set_pointer(field, blockOrig);
        stream_rewind(bs);
        size_t bits = zfp_compress(zs, field);
        return bits;
    }

    bool decompress() {
        zfp_field_set_pointer(fieldDec, blockDec);
        stream_rewind(bs);
        size_t ret = zfp_decompress(zs, fieldDec);
        return ret != 0;
    }
};

static std::string formatJsonDouble(double v) {
    std::ostringstream ss;
    ss << std::setprecision(15) << v;
    return ss.str();
}

// Thread-local ZFP state — each worker thread gets its own instance
static thread_local std::unique_ptr<ZfpThreadState> tlsZfpState;
static size_t g_compBufSize = 0;

static ZfpThreadState& getThreadZfpState() {
    if (!tlsZfpState) {
        tlsZfpState = std::make_unique<ZfpThreadState>();
        tlsZfpState->init(g_compBufSize);
    }
    return *tlsZfpState;
}

int main(int argc, char* argv[]) {
    ScanOptions opt;
    if (!parseArgs(argc, argv, opt)) return 1;

    int fd = open(opt.rawPath.c_str(), O_RDONLY);
    if (fd < 0) {
        std::cerr << "Error: cannot open " << opt.rawPath << ": " << strerror(errno) << "\n";
        return 1;
    }
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);

    uint64_t nx = opt.nx, ny = opt.ny, nz = opt.nz;
    int numRates = (int)opt.rates.size();
    uint64_t bxCount = (nx + BLOCK_DIM - 1) / BLOCK_DIM;
    uint64_t byCount = (ny + BLOCK_DIM - 1) / BLOCK_DIM;
    uint64_t bzCount = (nz + BLOCK_DIM - 1) / BLOCK_DIM;
    uint64_t totalBlocks = bxCount * byCount * bzCount;

    int slabZ = opt.slabZLayers;
    if (slabZ > (int)nz) slabZ = (int)nz;
    // Align slabZ to BLOCK_DIM for clean block extraction
    if (slabZ % BLOCK_DIM != 0) slabZ = (slabZ / BLOCK_DIM) * BLOCK_DIM;
    if (slabZ < (int)BLOCK_DIM) slabZ = BLOCK_DIM;

    uint64_t rowFloats = nx * ny;
    uint64_t slabBytes = rowFloats * (uint64_t)slabZ * sizeof(float);
    uint64_t slabBlocksPerLayer = bxCount * byCount;
    uint64_t slabBzCount = slabZ / BLOCK_DIM;

    // ZFP max compressed block size for fixed-rate: rate * 64 bits = rate * 8 bytes
    // Use generous buffer: max rate * 64 bits / 8 + overhead
    double maxRate = *std::max_element(opt.rates.begin(), opt.rates.end());
    size_t compBufSize = (size_t)(maxRate * 64.0 / 8.0) + 64;

    std::cerr << "ZFP Rate Scanner\n";
    std::cerr << "  raw: " << opt.rawPath << " (" << nx << "x" << ny << "x" << nz << ")\n";
    std::cerr << "  total blocks: " << totalBlocks << " (" << bxCount << "x" << byCount << "x" << bzCount << ")\n";
    std::cerr << "  rates: ";
    for (double r : opt.rates) std::cerr << r << " ";
    std::cerr << "bpp\n";
    std::cerr << "  threads: " << opt.threads << ", slab_z: " << slabZ << "\n";
    std::cerr << "  slab memory: " << (slabBytes / (1024.0 * 1024.0)) << " MB\n";

    // Per-rate statistics
    std::vector<RateStats> rateStats(numRates);
    for (int r = 0; r < numRates; ++r) {
        rateStats[r].rate = opt.rates[r];
        rateStats[r].totalBlocks = totalBlocks;
        // Reserve approximate capacity for blockMaxRelErrors
        // Use 90th percentile estimate to bound memory; will grow as needed
        rateStats[r].blockMaxRelErrors.reserve(totalBlocks);
    }

    // Slab buffer
    std::vector<float> slab(slabBytes / sizeof(float), 0.0f);

    // Thread pool — each worker thread gets its own ZFP state via thread_local
    g_compBufSize = compBufSize;
    erwt3d::ThreadPool pool(opt.threads);

    auto tStart = std::chrono::high_resolution_clock::now();

    uint64_t blocksScanned = 0;

    for (uint64_t zSlabStart = 0; zSlabStart < nz; zSlabStart += slabZ) {
        uint64_t zSlabEnd = std::min(zSlabStart + (uint64_t)slabZ, nz);
        uint64_t actualSlabZ = zSlabEnd - zSlabStart;

        // Read slab: sequential z-layer pread
        for (uint64_t z = zSlabStart; z < zSlabEnd; ++z) {
            uint64_t rowOff = z * rowFloats * sizeof(float);
            float* dst = slab.data() + (z - zSlabStart) * rowFloats;
            uint64_t remaining = rowFloats * sizeof(float);
            uint64_t off = rowOff;
            char* p = reinterpret_cast<char*>(dst);
            while (remaining > 0) {
                ssize_t rd = pread(fd, p, remaining, off);
                if (rd <= 0) {
                    std::cerr << "Error reading z-layer " << z << ": " << strerror(errno) << "\n";
                    close(fd);
                    return 1;
                }
                remaining -= rd;
                off += rd;
                p += rd;
            }
        }

        // Pad partial top z-layer if needed (for last slab with < slabZ layers)
        if (actualSlabZ < (uint64_t)slabZ) {
            float* padStart = slab.data() + actualSlabZ * rowFloats;
            std::memset(padStart, 0, (slabZ - actualSlabZ) * rowFloats * sizeof(float));
        }

        // Determine block ranges for this slab
        uint64_t bzSlabStart = zSlabStart / BLOCK_DIM;
        uint64_t bzSlabEnd = (zSlabEnd + BLOCK_DIM - 1) / BLOCK_DIM;
        if (bzSlabEnd > bzCount) bzSlabEnd = bzCount;

        // Collect all block indices in this slab
        struct BlockTask {
            uint64_t bx, by, bz;  // global block coords
        };
        std::vector<BlockTask> tasks;
        tasks.reserve(slabBlocksPerLayer * (bzSlabEnd - bzSlabStart));
        for (uint64_t bz = bzSlabStart; bz < bzSlabEnd; ++bz) {
            for (uint64_t by = 0; by < byCount; ++by) {
                for (uint64_t bx = 0; bx < bxCount; ++bx) {
                    tasks.push_back({bx, by, bz});
                }
            }
        }

        // Dispatch blocks to thread pool
        std::vector<std::future<BlockResult>> futures;
        futures.reserve(tasks.size());

        for (auto& task : tasks) {
            uint64_t bx = task.bx, by = task.by, bz = task.bz;
            uint64_t lzBase = (bz * BLOCK_DIM - zSlabStart);

            futures.push_back(pool.submit(
                [&, bx, by, bz, lzBase]() -> BlockResult {
                    ZfpThreadState& ts = getThreadZfpState();

                    // Extract 4x4x4 block from slab
                    // slab is indexed as slab[(localZ * ny + y) * nx + x]
                    for (uint64_t iz = 0; iz < BLOCK_DIM; ++iz) {
                        uint64_t localZ = lzBase + iz;
                        for (uint64_t iy = 0; iy < BLOCK_DIM; ++iy) {
                            uint64_t gy = by * BLOCK_DIM + iy;
                            if (gy >= ny) {
                                for (uint64_t ix = 0; ix < BLOCK_DIM; ++ix)
                                    ts.blockOrig[(iz * BLOCK_DIM + iy) * BLOCK_DIM + ix] = 0.0f;
                                continue;
                            }
                            for (uint64_t ix = 0; ix < BLOCK_DIM; ++ix) {
                                uint64_t gx = bx * BLOCK_DIM + ix;
                                float val = 0.0f;
                                if (gx < nx && localZ < (uint64_t)slabZ) {
                                    val = slab[localZ * rowFloats + gy * nx + gx];
                                }
                                ts.blockOrig[(iz * BLOCK_DIM + iy) * BLOCK_DIM + ix] = val;
                            }
                        }
                    }

                    BlockResult result;
                    result.maxRelErrors.resize(numRates);
                    result.isException.resize(numRates);
                    result.hasNearZeroFailure.resize(numRates);
                    result.hasRelErrFailure.resize(numRates);
                    result.compressedBits.resize(numRates);

                    for (int r = 0; r < numRates; ++r) {
                        size_t compBytes = ts.compressAtRate(opt.rates[r]);
                        result.compressedBits[r] = (double)(compBytes * 8);

                        bool ok = ts.decompress();

                        double blockMaxRelErr = 0.0;
                        bool anyFailed = false;
                        bool hasNearZeroFail = false;
                        bool hasRelErrFail = false;

                        if (ok) {
                            for (size_t i = 0; i < BLOCK_FLOATS; ++i) {
                                double relErr;
                                bool isNearZeroFail = false;
                                bool failed = isFailed(
                                    ts.blockOrig[i], ts.blockDec[i],
                                    opt.relTol, opt.zeroAbsTol, relErr, isNearZeroFail);
                                if (failed) {
                                    anyFailed = true;
                                    if (isNearZeroFail) hasNearZeroFail = true;
                                    else hasRelErrFail = true;
                                }
                                // Only track relErr for non-near-zero points
                                if (!isNearZeroFail && relErr > blockMaxRelErr)
                                    blockMaxRelErr = relErr;
                            }
                        } else {
                            anyFailed = true;
                            hasRelErrFail = true;
                            blockMaxRelErr = 1e18;
                        }

                        result.maxRelErrors[r] = blockMaxRelErr;
                        result.isException[r] = anyFailed;
                        result.hasNearZeroFailure[r] = hasNearZeroFail;
                        result.hasRelErrFailure[r] = hasRelErrFail;
                    }

                    return result;
                }
            ));
        }

        // Collect results
        for (auto& fut : futures) {
            BlockResult res = fut.get();
            for (int r = 0; r < numRates; ++r) {
                if (res.isException[r]) {
                    rateStats[r].exceptionBlocks++;
                    if (res.hasNearZeroFailure[r]) rateStats[r].nearZeroExceptionBlocks++;
                    if (res.hasRelErrFailure[r]) rateStats[r].relErrExceptionBlocks++;
                }
                if (res.maxRelErrors[r] > rateStats[r].maxRelError)
                    rateStats[r].maxRelError = res.maxRelErrors[r];
                rateStats[r].sumCompressedBits += res.compressedBits[r];
                rateStats[r].blockMaxRelErrors.push_back(res.maxRelErrors[r]);
            }
            blocksScanned++;
        }

        if (blocksScanned % 1000000 == 0 || zSlabEnd >= nz) {
            double elapsed = std::chrono::duration<double>(
                std::chrono::high_resolution_clock::now() - tStart).count();
            double pct = 100.0 * zSlabEnd / nz;
            std::cerr << "\r  scanned " << blocksScanned << "/" << totalBlocks
                      << " blocks (" << std::fixed << std::setprecision(1) << pct << "%)"
                      << " elapsed " << std::setprecision(1) << elapsed << "s" << std::flush;
        }
    }
    std::cerr << "\n";
    close(fd);

    auto tEnd = std::chrono::high_resolution_clock::now();
    double totalSec = std::chrono::duration<double>(tEnd - tStart).count();
    std::cerr << "Scan complete in " << totalSec << "s\n";

    // Compute p99 and output JSON
    std::ostringstream json;
    json << std::setprecision(15);
    json << "{\n";
    json << "  \"raw\": \"" << opt.rawPath << "\",\n";
    json << "  \"dimensions\": {\"nx\": " << nx << ", \"ny\": " << ny << ", \"nz\": " << nz << "},\n";
    json << "  \"total_blocks\": " << totalBlocks << ",\n";
    json << "  \"block_size\": [4, 4, 4],\n";
    json << "  \"params\": {\n";
    json << "    \"rel_tol\": " << formatJsonDouble(opt.relTol) << ",\n";
    json << "    \"zero_abs_tol\": " << formatJsonDouble(opt.zeroAbsTol) << ",\n";
    json << "    \"threads\": " << opt.threads << ",\n";
    json << "    \"slab_z_layers\": " << slabZ << "\n";
    json << "  },\n";
    json << "  \"scan_time_seconds\": " << totalSec << ",\n";
    json << "  \"rates\": [\n";

    for (int r = 0; r < numRates; ++r) {
        RateStats& rs = rateStats[r];
        double rate = rs.rate;
        double maxbitsPerBlock = rate * 64.0;
        double mainRatio3axis = (rate / 32.0) * 3.0;
        double exceptionRatio = (double)rs.exceptionBlocks / (double)rs.totalBlocks;
        double nearZeroExcRatio = (double)rs.nearZeroExceptionBlocks / (double)rs.totalBlocks;
        double relErrExcRatio = (double)rs.relErrExceptionBlocks / (double)rs.totalBlocks;
        double estimatedExceptionRawRatio = exceptionRatio;
        double estimatedTotalRatio = mainRatio3axis + estimatedExceptionRawRatio;
        double avgCompressedBits = rs.sumCompressedBits / (double)rs.totalBlocks;

        // p99
        double p99 = 0.0;
        if (!rs.blockMaxRelErrors.empty()) {
            std::vector<double> sorted = rs.blockMaxRelErrors;
            std::sort(sorted.begin(), sorted.end());
            size_t p99idx = (size_t)(sorted.size() * 0.99);
            if (p99idx >= sorted.size()) p99idx = sorted.size() - 1;
            p99 = sorted[p99idx];
        }

        json << "    {\n";
        json << "      \"rate\": " << formatJsonDouble(rate) << ",\n";
        json << "      \"maxbits_per_block\": " << formatJsonDouble(maxbitsPerBlock) << ",\n";
        json << "      \"main_ratio_3axis\": " << formatJsonDouble(mainRatio3axis) << ",\n";
        json << "      \"exception_ratio\": " << formatJsonDouble(exceptionRatio) << ",\n";
        json << "      \"near_zero_exception_ratio\": " << formatJsonDouble(nearZeroExcRatio) << ",\n";
        json << "      \"rel_err_exception_ratio\": " << formatJsonDouble(relErrExcRatio) << ",\n";
        json << "      \"estimated_exception_raw_ratio\": " << formatJsonDouble(estimatedExceptionRawRatio) << ",\n";
        json << "      \"estimated_total_ratio\": " << formatJsonDouble(estimatedTotalRatio) << ",\n";
        json << "      \"max_rel_error\": " << formatJsonDouble(rs.maxRelError) << ",\n";
        json << "      \"p99_rel_error\": " << formatJsonDouble(p99) << ",\n";
        json << "      \"avg_compressed_bits\": " << formatJsonDouble(avgCompressedBits) << "\n";
        if (r < numRates - 1) json << "    },\n";
        else json << "    }\n";
    }
    json << "  ]\n";
    json << "}\n";

    if (opt.outPath.empty()) {
        std::cout << json.str();
    } else {
        std::ofstream ofs(opt.outPath);
        if (!ofs) {
            std::cerr << "Error: cannot write to " << opt.outPath << "\n";
            return 1;
        }
        ofs << json.str();
        std::cerr << "Report written to " << opt.outPath << "\n";
    }

    // Free blockMaxRelErrors memory before exit
    for (auto& rs : rateStats) {
        std::vector<double>().swap(rs.blockMaxRelErrors);
    }

    return 0;
}
