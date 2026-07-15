#include "erwt3d/rzfp_reader.hpp"
#include "erwt3d/relative_error.hpp"
#include "erwt3d/morton.hpp"
#include "erwt3d/rzfp_codec.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

using namespace erwt3d;

namespace {

struct VerifyOptions {
    std::string raw_path;
    std::string rzfp_path;
    uint64_t nx = 0;
    uint64_t ny = 0;
    uint64_t nz = 0;
    bool full = false;
    bool fast_full = false;
    uint64_t samples = 0;
    std::string error_policy = "strict";
    double contest_rel_bound = 1e-3;
};

static bool readFullyAt(int fd, void* buffer, size_t bytes, uint64_t offset) {
    auto* dst = static_cast<uint8_t*>(buffer);
    size_t done = 0;
    while (done < bytes) {
        ssize_t n = pread(fd, dst + done, bytes - done, static_cast<off_t>(offset + done));
        if (n == 0) return false;
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        done += static_cast<size_t>(n);
    }
    return true;
}

static erwt3d::RelativeErrorConfig makeConfig(const VerifyOptions& opt) {
    erwt3d::RelativeErrorConfig cfg;
    cfg.contest_bound = opt.contest_rel_bound;
    cfg.internal_bound = 7.5e-4;
    cfg.policy = (opt.error_policy == "legacy")
                     ? erwt3d::RelativeErrorPolicy::Legacy
                     : erwt3d::RelativeErrorPolicy::Strict;
    return cfg;
}

static uint64_t physicalSuperblockIdFast(const RzfpFileHeader& rh, uint64_t logical_id) {
    const uint64_t sgX = rzfpSuperGridX(rh);
    const uint64_t sgY = rzfpSuperGridY(rh);
    const uint64_t sx = logical_id % sgX;
    const uint64_t rem = logical_id / sgX;
    const uint64_t sy = rem % sgY;
    const uint64_t sz = rem / sgY;
    return rzfpSuperblockId(rh, sz, sy, sx,
                            (rh.flags & FLAG_PHYSICAL_ORDER_YZX) ? PhysicalOrder::V05_YZX : PhysicalOrder::ZYX);
}

static uint64_t buildValidMaskFast(
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

static int runFastFullVerify(const VerifyOptions& opt) {
    int rzfpFd = open(opt.rzfp_path.c_str(), O_RDONLY);
    if (rzfpFd < 0) {
        std::cerr << "Error: cannot open RZFP file" << std::endl;
        return 1;
    }

    RzfpFileHeader header{};
    if (pread(rzfpFd, &header, sizeof(header), 0) != sizeof(header) || !validateRzfpHeader(header)) {
        std::cerr << "Error: invalid RZFP header" << std::endl;
        close(rzfpFd);
        return 1;
    }

    const uint64_t totalSB = rzfpTotalSuperblocks(header);
    const uint64_t totalLeaves = rzfpTotalLeaves(header);
    std::vector<RzfpSuperblockIndex> sbIndex(totalSB);
    if (!readFullyAt(rzfpFd, sbIndex.data(), totalSB * sizeof(RzfpSuperblockIndex), sizeof(RzfpFileHeader))) {
        std::cerr << "Error: cannot read superblock index" << std::endl;
        close(rzfpFd);
        return 1;
    }
    std::vector<RzfpLeafDescriptor> descriptors(totalLeaves);
    if (!readFullyAt(rzfpFd, descriptors.data(), totalLeaves * sizeof(RzfpLeafDescriptor), header.descriptor_offset)) {
        std::cerr << "Error: cannot read descriptors" << std::endl;
        close(rzfpFd);
        return 1;
    }

    int rawFd = open(opt.raw_path.c_str(), O_RDONLY);
    if (rawFd < 0) {
        std::cerr << "Error: cannot open raw file" << std::endl;
        close(rzfpFd);
        return 1;
    }

    const auto cfg = makeConfig(opt);
    const uint64_t sgX = rzfpSuperGridX(header);
    const uint64_t sgY = rzfpSuperGridY(header);
    const uint64_t sgZ = rzfpSuperGridZ(header);
    const uint64_t leavesPerSB = rzfpTotalLeafsPerSuper(header);
    const uint64_t superSize = header.super_x;

    std::vector<float> slab;
    std::vector<uint8_t> payloadBuf;
    std::vector<uint32_t> prefix(leavesPerSB + 1);

    RzfpCodec codec;
    float leaf[64];

    uint64_t failed = 0;
    double max_abs = 0.0;
    double max_rel = 0.0;
    uint64_t checked = 0;
    const uint64_t totalSuperblocks = sgX * sgY * sgZ;
    const uint64_t progressStep = std::max<uint64_t>(1, totalSuperblocks / 100);

    const uint64_t yzFloats = opt.ny * opt.nz;

    for (uint64_t sx = 0; sx < sgX; ++sx) {
        const uint64_t xStart = sx * superSize;
        const uint64_t currentX = std::min<uint64_t>(superSize, opt.nx - xStart);
        const uint64_t slabFloats = currentX * yzFloats;
        slab.resize(slabFloats);
        const uint64_t slabBytes = slabFloats * sizeof(float);
        if (!readFullyAt(rawFd, slab.data(), slabBytes, xStart * yzFloats * sizeof(float))) {
            std::cerr << "Error: failed to read raw X-slab x=" << xStart << std::endl;
            close(rawFd);
            close(rzfpFd);
            return 1;
        }

        for (uint64_t sz = 0; sz < sgZ; ++sz) {
            for (uint64_t sy = 0; sy < sgY; ++sy) {
                const uint64_t logical_id = (sz * sgY + sy) * sgX + sx;
                const uint64_t phys = physicalSuperblockIdFast(header, logical_id);
                const uint64_t base_x = sx * superSize;
                const uint64_t base_y = sy * superSize;
                const uint64_t base_z = sz * superSize;

                const auto& sb = sbIndex[phys];
                payloadBuf.resize(sb.payload_bytes);
                if (sb.payload_bytes > 0 && !readFullyAt(rzfpFd, payloadBuf.data(), sb.payload_bytes, sb.payload_offset)) {
                    std::cerr << "Error: failed to read payload for sb=" << phys << std::endl;
                    close(rawFd);
                    close(rzfpFd);
                    return 1;
                }

                prefix[0] = 0;
                const uint64_t descBase = phys * leavesPerSB;
                for (uint64_t i = 0; i < leavesPerSB; ++i) {
                    prefix[i + 1] = prefix[i] + descriptorSize(descriptors[descBase + i]);
                }

                for (uint64_t j = 0; j < leavesPerSB; ++j) {
                    const RzfpLeafDescriptor descriptor = descriptors[descBase + j];
                    const RzfpLeafCodec codec_type = descriptorCodec(descriptor);
                    const uint16_t record_size = descriptorSize(descriptor);

                    RzfpCandidate cand;
                    cand.codec = codec_type;
                    cand.payload.assign(payloadBuf.data() + prefix[j],
                                        payloadBuf.data() + prefix[j] + record_size);

                    if (!codec.decode(cand, leaf)) {
                        std::cerr << "Error: RZFP decode failed for sb=" << phys << " leaf=" << j << std::endl;
                        close(rawFd);
                        close(rzfpFd);
                        return 1;
                    }

                    uint32_t lx, ly, lz;
                    unmorton3D(static_cast<uint32_t>(j), lx, ly, lz);
                    const uint64_t start_x = base_x + static_cast<uint64_t>(lx) * header.leaf_x;
                    const uint64_t start_y = base_y + static_cast<uint64_t>(ly) * header.leaf_y;
                    const uint64_t start_z = base_z + static_cast<uint64_t>(lz) * header.leaf_z;
                    const uint64_t valid_mask = buildValidMaskFast(start_x, start_y, start_z, opt.nx, opt.ny, opt.nz);

                    for (uint32_t z = 0; z < header.leaf_z; ++z) {
                        for (uint32_t y = 0; y < header.leaf_y; ++y) {
                            for (uint32_t x = 0; x < header.leaf_x; ++x) {
                                const uint32_t idx = (z * header.leaf_y + y) * header.leaf_x + x;
                                if ((valid_mask & (uint64_t{1} << idx)) == 0) continue;
                                const uint64_t gx = start_x + x;
                                const uint64_t gy = start_y + y;
                                const uint64_t gz = start_z + z;
                                const uint64_t local_x = gx - xStart;
                                const float raw_val = slab[local_x * yzFloats + gy * opt.nz + gz];
                                const float dec_val = leaf[idx];
                                auto r = erwt3d::checkPointwiseError(raw_val, dec_val, cfg);
                                ++checked;
                                if (!r.passed) ++failed;
                                max_abs = std::max(max_abs, r.absolute_error);
                                max_rel = std::max(max_rel, r.relative_error);
                            }
                        }
                    }
                }

                if (logical_id % progressStep == 0 || logical_id + 1 == totalSuperblocks) {
                    std::cerr << "\rFast verify progress: " << (100 * (logical_id + 1) / totalSuperblocks) << "%" << std::flush;
                }
            }
        }
    }
    std::cerr << std::endl;

    close(rawFd);
    close(rzfpFd);

    std::cout << "checked: " << checked << std::endl;
    std::cout << "failed: " << failed << std::endl;
    std::cout << "max_abs_error: " << max_abs << std::endl;
    std::cout << "max_rel_error: " << max_rel << std::endl;
    std::cout << "passed: " << (failed == 0 ? "true" : "false") << std::endl;
    return failed == 0 ? 0 : 1;
}

static int runFullVerify(const VerifyOptions& opt) {
    erwt3d::RzfpReader reader(opt.rzfp_path);
    if (!reader.ok()) {
        std::cerr << "Error: cannot open RZFP file" << std::endl;
        return 1;
    }

    const auto cfg = makeConfig(opt);
    std::vector<float> slice(opt.nx * opt.ny);

    int rawFd = open(opt.raw_path.c_str(), O_RDONLY);
    if (rawFd < 0) {
        std::cerr << "Error: cannot open raw file" << std::endl;
        return 1;
    }

    const uint64_t totalFloats = opt.nx * opt.ny * opt.nz;
    std::vector<float> raw_volume;
    raw_volume.resize(totalFloats);
    if (!readFullyAt(rawFd, raw_volume.data(), totalFloats * sizeof(float), 0)) {
        std::cerr << "Error: failed to read whole raw file" << std::endl;
        close(rawFd);
        return 1;
    }
    close(rawFd);

    uint64_t failed = 0;
    double max_abs = 0.0;
    double max_rel = 0.0;
    uint64_t checked = 0;

    const uint64_t yzFloats = opt.ny * opt.nz;

    for (uint64_t z = 0; z < opt.nz; ++z) {
        if (!reader.readSlice(erwt3d::SliceAxis::Z, z, slice.data())) {
            std::cerr << "Error: failed to read Z slice " << z << std::endl;
            return 1;
        }

        for (uint64_t y = 0; y < opt.ny; ++y) {
            for (uint64_t x = 0; x < opt.nx; ++x) {
                const uint64_t raw_idx = x * yzFloats + y * opt.nz + z;
                const float raw_val = raw_volume[raw_idx];
                const float dec_val = slice[y * opt.nx + x];
                auto r = erwt3d::checkPointwiseError(raw_val, dec_val, cfg);
                ++checked;
                if (!r.passed) ++failed;
                max_abs = std::max(max_abs, r.absolute_error);
                max_rel = std::max(max_rel, r.relative_error);
            }
        }

        if (z % 10 == 0 || z + 1 == opt.nz) {
            std::cerr << "\rVerify progress: " << (100 * (z + 1) / opt.nz) << "%" << std::flush;
        }
    }
    std::cerr << std::endl;

    std::cout << "checked: " << checked << std::endl;
    std::cout << "failed: " << failed << std::endl;
    std::cout << "max_abs_error: " << max_abs << std::endl;
    std::cout << "max_rel_error: " << max_rel << std::endl;
    std::cout << "passed: " << (failed == 0 ? "true" : "false") << std::endl;
    return failed == 0 ? 0 : 1;
}

static int runSampleVerify(const VerifyOptions& opt) {
    erwt3d::RzfpReader reader(opt.rzfp_path);
    if (!reader.ok()) {
        std::cerr << "Error: cannot open RZFP file" << std::endl;
        return 1;
    }

    const auto cfg = makeConfig(opt);
    const uint64_t total = opt.nx * opt.ny * opt.nz;
    std::mt19937_64 rng(20260511);
    std::uniform_int_distribution<uint64_t> dist(0, total - 1);

    struct Sample { uint64_t x, y, z; };
    std::vector<Sample> samples;
    samples.reserve(opt.samples);
    for (uint64_t i = 0; i < opt.samples; ++i) {
        uint64_t v = dist(rng);
        uint64_t x = v % opt.nx;
        uint64_t y = (v / opt.nx) % opt.ny;
        uint64_t z = v / (opt.nx * opt.ny);
        samples.push_back({x, y, z});
    }

    std::sort(samples.begin(), samples.end(), [](const Sample& a, const Sample& b) {
        return a.z < b.z;
    });

    int rawFd = open(opt.raw_path.c_str(), O_RDONLY);
    if (rawFd < 0) {
        std::cerr << "Error: cannot open raw file" << std::endl;
        return 1;
    }

    std::vector<float> slice(opt.nx * opt.ny);
    uint64_t failed = 0;
    double max_abs = 0.0;
    double max_rel = 0.0;
    uint64_t checked = 0;

    size_t i = 0;
    while (i < samples.size()) {
        uint64_t z = samples[i].z;
        if (!reader.readSlice(erwt3d::SliceAxis::Z, z, slice.data())) {
            std::cerr << "Error: failed to read Z slice " << z << std::endl;
            close(rawFd);
            return 1;
        }
        while (i < samples.size() && samples[i].z == z) {
            const auto& s = samples[i];
            float raw_val = 0.0f;
            const uint64_t off = s.x * opt.ny * opt.nz + s.y * opt.nz + s.z;
            if (!readFullyAt(rawFd, &raw_val, sizeof(float), off * sizeof(float))) {
                std::cerr << "Error reading raw sample" << std::endl;
                close(rawFd);
                return 1;
            }
            float dec_val = slice[s.y * opt.nx + s.x];
            auto r = erwt3d::checkPointwiseError(raw_val, dec_val, cfg);
            ++checked;
            if (!r.passed) ++failed;
            max_abs = std::max(max_abs, r.absolute_error);
            max_rel = std::max(max_rel, r.relative_error);
            ++i;
        }
    }
    close(rawFd);

    std::cout << "checked: " << checked << std::endl;
    std::cout << "failed: " << failed << std::endl;
    std::cout << "max_abs_error: " << max_abs << std::endl;
    std::cout << "max_rel_error: " << max_rel << std::endl;
    std::cout << "passed: " << (failed == 0 ? "true" : "false") << std::endl;
    return failed == 0 ? 0 : 1;
}

static void printUsage(const char* prog) {
    std::cerr << "Usage: " << prog << " --raw PATH --rzfp PATH --nx N --ny N --nz N [options]\n"
              << "Options:\n"
              << "  --full              Full slice-by-slice verification (slow)\n"
              << "  --fast-full         Full superblock-by-superblock verification\n"
              << "  --samples N         Random sample verification\n"
              << "  --error-policy strict|legacy (default: strict)\n"
              << "  --contest-rel-bound V (default: 0.001)\n";
}

} // namespace

int main(int argc, char* argv[]) {
    VerifyOptions opt;
    for (int i = 1; i < argc; ++i) {
        auto next = [&]() -> const char* {
            if (i + 1 < argc) return argv[++i];
            std::cerr << "Error: " << argv[i] << " requires a value\n";
            std::exit(1);
            return nullptr;
        };
        if (std::strcmp(argv[i], "--raw") == 0) opt.raw_path = next();
        else if (std::strcmp(argv[i], "--rzfp") == 0) opt.rzfp_path = next();
        else if (std::strcmp(argv[i], "--nx") == 0) opt.nx = std::stoull(next());
        else if (std::strcmp(argv[i], "--ny") == 0) opt.ny = std::stoull(next());
        else if (std::strcmp(argv[i], "--nz") == 0) opt.nz = std::stoull(next());
        else if (std::strcmp(argv[i], "--full") == 0) opt.full = true;
        else if (std::strcmp(argv[i], "--fast-full") == 0) opt.fast_full = true;
        else if (std::strcmp(argv[i], "--samples") == 0) opt.samples = std::stoull(next());
        else if (std::strcmp(argv[i], "--error-policy") == 0) opt.error_policy = next();
        else if (std::strcmp(argv[i], "--contest-rel-bound") == 0) opt.contest_rel_bound = std::stod(next());
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]); return 0;
        } else {
            std::cerr << "Unknown option: " << argv[i] << std::endl;
            printUsage(argv[0]); return 1;
        }
    }

    if (opt.raw_path.empty() || opt.rzfp_path.empty() || opt.nx == 0 || opt.ny == 0 || opt.nz == 0) {
        std::cerr << "Error: --raw, --rzfp, --nx, --ny, --nz are required\n";
        printUsage(argv[0]);
        return 1;
    }

    if (opt.fast_full) return runFastFullVerify(opt);
    if (opt.full) return runFullVerify(opt);
    if (opt.samples > 0) return runSampleVerify(opt);

    std::cerr << "Error: specify --full or --samples" << std::endl;
    return 1;
}
