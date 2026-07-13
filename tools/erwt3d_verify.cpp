#include "erwt3d/reader.hpp"
#include "erwt3d/morton.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#ifdef ERWT3D_HAVE_LZ4
#include <lz4.h>
#endif

namespace {

struct VerifyOptions {
    std::string rawPath;
    std::string erwt3dPath;
    std::string rawAPath;
    std::string rawBPath;
    uint64_t nx = 0;
    uint64_t ny = 0;
    uint64_t nz = 0;
    uint64_t numSamples = 0;
    uint64_t seed = 20260511;
    double relTol = 1e-3;
    double zeroAbsTol = 1e-6;
    bool strictRelative = false;
};

struct VerifyStats {
    double maxAbsError = 0.0;
    double maxRelError = 0.0;
    uint64_t numFailed = 0;
};

void printUsage(const char* progName) {
    std::cerr
        << "Usage:\n"
        << "  Compare raw file with ERWT3D:\n"
        << "    " << progName
        << " --raw data.raw --erwt3d data.erwt3d --nx N --ny N --nz N [--samples N]\n\n"
        << "  Compare two raw files:\n"
        << "    " << progName
        << " --raw-a data.raw --raw-b restored.raw --nx N --ny N --nz N\n\n"
        << "Error options:\n"
        << "  --rel-tol VALUE          Relative error threshold (default: 1e-3)\n"
        << "  --zero-abs-tol VALUE     Absolute tolerance for near-zero reference values (default: 1e-6)\n"
        << "  --seed N                 Sampling seed for streaming mode (default: 20260511)\n"
        << "  --strict-relative        Apply relative error check to all points, including near-zero values\n";
}

bool isFailed(float refVal, float actualVal, const VerifyOptions& opt, double& absErr, double& relErr) {
    absErr = std::abs(static_cast<double>(refVal) - static_cast<double>(actualVal));
    double absRef = std::abs(static_cast<double>(refVal));
    relErr = absErr / std::max(absRef, 1e-12);

    if (absRef <= opt.zeroAbsTol && !opt.strictRelative) {
        return absErr > opt.zeroAbsTol;
    }
    return relErr >= opt.relTol;
}

void updateStats(VerifyStats& stats, float refVal, float actualVal, const VerifyOptions& opt) {
    double absErr = 0.0;
    double relErr = 0.0;
    bool failed = isFailed(refVal, actualVal, opt, absErr, relErr);
    stats.maxAbsError = std::max(stats.maxAbsError, absErr);
    stats.maxRelError = std::max(stats.maxRelError, relErr);
    if (failed) {
        ++stats.numFailed;
    }
}

void printStats(const VerifyStats& stats, const VerifyOptions& opt) {
    std::cout << "max_abs_error: " << stats.maxAbsError << std::endl;
    std::cout << "max_rel_error: " << stats.maxRelError << std::endl;
    std::cout << "seed: " << opt.seed << std::endl;
    std::cout << "rel_tol: " << opt.relTol << std::endl;
    std::cout << "zero_abs_tol: " << opt.zeroAbsTol << std::endl;
    std::cout << "strict_relative: " << (opt.strictRelative ? "true" : "false") << std::endl;
    std::cout << "num_failed: " << stats.numFailed << std::endl;
    std::cout << "passed: " << (stats.numFailed == 0 ? "true" : "false") << std::endl;
}

bool compareRawVsErwt3d(const VerifyOptions& opt, VerifyStats& stats) {
    uint64_t totalElements = opt.nx * opt.ny * opt.nz;

    if (opt.numSamples > 0 && opt.numSamples < totalElements) {
        std::cout << "Comparing raw file with ERWT3D..." << std::endl;
        std::cout << "Using streaming sampling mode (" << opt.numSamples << " samples)" << std::endl;

        erwt3d::ERWT3DReader reader(opt.erwt3dPath);
        const auto& hdr = reader.getHeader();
        uint64_t superBytes = erwt3d::getSuperblockBytes(hdr);
        uint64_t leafBytes = erwt3d::getLeafBytes(hdr);
        uint64_t gridX = erwt3d::getSuperGridX(hdr);
        uint64_t gridY = erwt3d::getSuperGridY(hdr);

        int erwt3dFd = open(opt.erwt3dPath.c_str(), O_RDONLY);
        if (erwt3dFd < 0) {
            std::cerr << "Error: Cannot open ERWT3D file" << std::endl;
            return false;
        }

        std::mt19937_64 rng(opt.seed);
        std::uniform_int_distribution<uint64_t> dist(0, totalElements - 1);
        uint64_t nxy = static_cast<uint64_t>(opt.nx) * opt.ny;

        struct Sample {
            uint64_t linIdx;
            uint64_t sbIdx;
            uint64_t byteOffSb;
            float rawVal;
        };
        std::vector<Sample> samples;
        samples.reserve(opt.numSamples);

        for (uint64_t s = 0; s < opt.numSamples; ++s) {
            uint64_t idx = dist(rng);
            uint64_t x = idx % opt.nx;
            uint64_t y = (idx / opt.nx) % opt.ny;
            uint64_t z = idx / nxy;

            uint64_t sx = x / hdr.super_x;
            uint64_t syLocal = y / hdr.super_y;
            uint64_t sz = z / hdr.super_z;
            uint64_t sbIdx = (sz * gridY + syLocal) * gridX + sx;

            uint64_t lx = x % hdr.super_x;
            uint64_t ly = y % hdr.super_y;
            uint64_t lz = z % hdr.super_z;

            uint64_t leafLx = lx / hdr.leaf_x;
            uint64_t leafLy = ly / hdr.leaf_y;
            uint64_t leafLz = lz / hdr.leaf_z;
            uint64_t leafMorton = erwt3d::morton3D(
                static_cast<uint32_t>(leafLx),
                static_cast<uint32_t>(leafLy),
                static_cast<uint32_t>(leafLz));

            uint64_t inLeafX = lx % hdr.leaf_x;
            uint64_t inLeafY = ly % hdr.leaf_y;
            uint64_t inLeafZ = lz % hdr.leaf_z;
            uint64_t elemOff = (inLeafZ * hdr.leaf_y + inLeafY) * hdr.leaf_x + inLeafX;
            uint64_t byteOffsetInSb = leafMorton * leafBytes + elemOff * sizeof(float);

            samples.push_back({idx, sbIdx, byteOffsetInSb, 0.0f});
        }

        std::sort(samples.begin(), samples.end(),
                  [](const Sample& a, const Sample& b) { return a.linIdx < b.linIdx; });

        std::cout << "Reading raw file (mmap)..." << std::endl;
        int rawFd = open(opt.rawPath.c_str(), O_RDONLY);
        if (rawFd < 0) {
            std::cerr << "Error: Cannot open raw file" << std::endl;
            close(erwt3dFd);
            return false;
        }

        struct stat rawStat;
        fstat(rawFd, &rawStat);
        uint64_t rawSize = static_cast<uint64_t>(rawStat.st_size);
        void* rawMap = mmap(nullptr, rawSize, PROT_READ, MAP_PRIVATE | MAP_POPULATE, rawFd, 0);
        if (rawMap == MAP_FAILED) {
            std::cerr << "Error: mmap raw file failed" << std::endl;
            close(rawFd);
            close(erwt3dFd);
            return false;
        }
        madvise(rawMap, rawSize, MADV_SEQUENTIAL);
        const float* rawFloats = static_cast<const float*>(rawMap);

        uint64_t totalElems = opt.nx * opt.ny * opt.nz;
        for (auto& s : samples) {
            s.rawVal = rawFloats[s.linIdx];
        }

        munmap(rawMap, rawSize);
        close(rawFd);

        std::sort(samples.begin(), samples.end(),
                  [](const Sample& a, const Sample& b) { return a.sbIdx < b.sbIdx; });

        std::cout << "Reading ERWT3D superblocks..." << std::endl;
        std::vector<uint8_t> sbBuf(superBytes);
        bool compressed = erwt3d::isCompressed(hdr);
        std::vector<erwt3d::CompressedBlockIndex> compIdx;
        if (compressed) {
            uint64_t idxOff = erwt3d::getCompressionIndexOffset(hdr);
            uint64_t idxCnt = erwt3d::getCompressedBlockCount(hdr);
            compIdx.resize(idxCnt);
            pread(erwt3dFd, compIdx.data(), idxCnt * sizeof(erwt3d::CompressedBlockIndex), idxOff);
        }
        std::vector<uint8_t> compBuf(compressed ? superBytes : 0);

        uint64_t totalSamples = samples.size();
        uint64_t prevSbIdx = UINT64_MAX;
        size_t passIdx = 0;
        while (passIdx < samples.size()) {
            uint64_t sbIdx = samples[passIdx].sbIdx;
            bool needReload = (sbIdx != prevSbIdx);
            if (needReload) {
                if (compressed && sbIdx < compIdx.size()) {
                    const auto& entry = compIdx[sbIdx];
                    if (entry.is_compressed) {
#ifdef ERWT3D_HAVE_LZ4
                        if (compBuf.size() < entry.compressed_size) {
                            compBuf.resize(entry.compressed_size);
                        }
                        if (pread(erwt3dFd, compBuf.data(), entry.compressed_size, entry.file_offset) !=
                            static_cast<ssize_t>(entry.compressed_size)) {
                            std::cerr << "Error: failed to read compressed block " << sbIdx << std::endl;
                            close(erwt3dFd);
                            return false;
                        }
                        if (LZ4_decompress_safe(
                                reinterpret_cast<const char*>(compBuf.data()),
                                reinterpret_cast<char*>(sbBuf.data()),
                                entry.compressed_size,
                                superBytes) != static_cast<int>(superBytes)) {
                            std::cerr << "Error: failed to decompress block " << sbIdx << std::endl;
                            close(erwt3dFd);
                            return false;
                        }
#else
                        std::cerr << "Error: lz4 not available" << std::endl;
                        close(erwt3dFd);
                        return false;
#endif
                    } else if (pread(erwt3dFd, sbBuf.data(), superBytes, entry.file_offset) !=
                               static_cast<ssize_t>(superBytes)) {
                        std::cerr << "Error: failed to read block " << sbIdx << std::endl;
                        close(erwt3dFd);
                        return false;
                    }
                } else {
                    uint64_t sbOff = hdr.data_offset + sbIdx * superBytes;
                    if (pread(erwt3dFd, sbBuf.data(), superBytes, sbOff) != static_cast<ssize_t>(superBytes)) {
                        std::cerr << "Error: failed to read superblock " << sbIdx << std::endl;
                        close(erwt3dFd);
                        return false;
                    }
                }
                prevSbIdx = sbIdx;
            }

            for (; passIdx < samples.size() && samples[passIdx].sbIdx == sbIdx; ++passIdx) {
                float erwt3dVal = *reinterpret_cast<const float*>(sbBuf.data() + samples[passIdx].byteOffSb);
                updateStats(stats, samples[passIdx].rawVal, erwt3dVal, opt);
            }

            if (passIdx % (totalSamples / 40 + 1) < 64) {
                std::cerr << "\rVerify progress: " << (passIdx * 100 / totalSamples) << "%" << std::flush;
            }
        }
        std::cerr << "\rVerify progress: 100%" << std::endl;

        close(erwt3dFd);
        return true;
    }

    std::cout << "Comparing raw file with ERWT3D..." << std::endl;

    std::ifstream rawFile(opt.rawPath, std::ios::binary);
    if (!rawFile) {
        std::cerr << "Error: Cannot open raw file" << std::endl;
        return false;
    }

    std::vector<float> rawData(totalElements);
    rawFile.read(reinterpret_cast<char*>(rawData.data()), totalElements * sizeof(float));
    if (!rawFile) {
        std::cerr << "Error: Failed to read raw file" << std::endl;
        return false;
    }

    erwt3d::ERWT3DReader reader(opt.erwt3dPath);
    std::vector<float> erwt3dData(totalElements);
    if (!reader.readFull(erwt3dData.data())) {
        std::cerr << "Error: Failed to read ERWT3D file" << std::endl;
        return false;
    }

    for (uint64_t i = 0; i < totalElements; ++i) {
        updateStats(stats, rawData[i], erwt3dData[i], opt);
    }
    return true;
}

bool compareRawVsRaw(const VerifyOptions& opt, VerifyStats& stats) {
    uint64_t totalElements = opt.nx * opt.ny * opt.nz;
    std::cout << "Comparing two raw files..." << std::endl;

    std::ifstream rawAFile(opt.rawAPath, std::ios::binary);
    std::ifstream rawBFile(opt.rawBPath, std::ios::binary);
    if (!rawAFile || !rawBFile) {
        std::cerr << "Error: Cannot open raw files" << std::endl;
        return false;
    }

    std::vector<float> rawA(totalElements);
    std::vector<float> rawB(totalElements);
    rawAFile.read(reinterpret_cast<char*>(rawA.data()), totalElements * sizeof(float));
    rawBFile.read(reinterpret_cast<char*>(rawB.data()), totalElements * sizeof(float));
    if (!rawAFile || !rawBFile) {
        std::cerr << "Error: Failed to read raw files" << std::endl;
        return false;
    }

    for (uint64_t i = 0; i < totalElements; ++i) {
        updateStats(stats, rawA[i], rawB[i], opt);
    }
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    VerifyOptions opt;

    for (int i = 1; i < argc; ++i) {
        auto next = [&]() -> const char* {
            if (i + 1 < argc) {
                return argv[++i];
            }
            std::cerr << "Error: " << argv[i] << " requires a value" << std::endl;
            std::exit(1);
            return nullptr;
        };

        if (std::strcmp(argv[i], "--raw") == 0) {
            opt.rawPath = next();
        } else if (std::strcmp(argv[i], "--erwt3d") == 0) {
            opt.erwt3dPath = next();
        } else if (std::strcmp(argv[i], "--raw-a") == 0) {
            opt.rawAPath = next();
        } else if (std::strcmp(argv[i], "--raw-b") == 0) {
            opt.rawBPath = next();
        } else if (std::strcmp(argv[i], "--nx") == 0) {
            opt.nx = std::stoull(next());
        } else if (std::strcmp(argv[i], "--ny") == 0) {
            opt.ny = std::stoull(next());
        } else if (std::strcmp(argv[i], "--nz") == 0) {
            opt.nz = std::stoull(next());
        } else if (std::strcmp(argv[i], "--samples") == 0) {
            opt.numSamples = std::stoull(next());
        } else if (std::strcmp(argv[i], "--seed") == 0) {
            opt.seed = std::stoull(next());
        } else if (std::strcmp(argv[i], "--rel-tol") == 0) {
            opt.relTol = std::stod(next());
        } else if (std::strcmp(argv[i], "--zero-abs-tol") == 0) {
            opt.zeroAbsTol = std::stod(next());
        } else if (std::strcmp(argv[i], "--strict-relative") == 0) {
            opt.strictRelative = true;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << argv[i] << std::endl;
            printUsage(argv[0]);
            return 1;
        }
    }

    if (opt.nx == 0 || opt.ny == 0 || opt.nz == 0) {
        std::cerr << "Error: --nx, --ny, --nz are required" << std::endl;
        printUsage(argv[0]);
        return 1;
    }

    VerifyStats stats;
    bool ok = false;
    if (!opt.rawPath.empty() && !opt.erwt3dPath.empty()) {
        ok = compareRawVsErwt3d(opt, stats);
    } else if (!opt.rawAPath.empty() && !opt.rawBPath.empty()) {
        ok = compareRawVsRaw(opt, stats);
    } else {
        std::cerr << "Error: Must specify either --raw/--erwt3d or --raw-a/--raw-b" << std::endl;
        printUsage(argv[0]);
        return 1;
    }

    if (!ok) {
        return 1;
    }

    printStats(stats, opt);
    return stats.numFailed == 0 ? 0 : 1;
}
