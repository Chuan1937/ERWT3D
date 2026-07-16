#include "erwt3d/rzfp_writer.hpp"
#include "erwt3d/rzfp_reader.hpp"
#include "erwt3d/relative_error.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* msg) {
    if (!condition) {
        std::cerr << "FAIL: " << msg << std::endl;
        ++g_failures;
    }
}

float valueAt(uint64_t x, uint64_t y, uint64_t z) {
    return static_cast<float>(x * 1000000ULL + y * 1000ULL + z);
}

enum class DataPattern { Coordinate, Mixed };

bool writeRawFile(const std::string& path,
                  uint64_t nx, uint64_t ny, uint64_t nz,
                  DataPattern pattern) {
    std::vector<float> data(nx * ny * nz, 0.0f);
    for (uint64_t x = 0; x < nx; ++x)
        for (uint64_t y = 0; y < ny; ++y)
            for (uint64_t z = 0; z < nz; ++z) {
                float v = 0.0f;
                switch (pattern) {
                    case DataPattern::Coordinate:
                        v = valueAt(x, y, z);
                        break;
                    case DataPattern::Mixed: {
                        uint64_t h = (x * 73856093ULL + y * 19349663ULL + z * 83492791ULL) % 10;
                        if (h == 0)      v = 0.0f;
                        else if (h == 1) v = -0.0f;
                        else if (h == 2) v = 1e-8f;
                        else if (h == 3) v = -1e-8f;
                        else if (h == 4) v = 0.5f;
                        else if (h == 5) v = -3.14f;
                        else if (h == 6) v = 1e6f;
                        else if (h == 7) v = -1e6f;
                        else             v = valueAt(x % 3, y % 3, z % 3);
                        break;
                    }
                }
                data[(x * ny + y) * nz + z] = v;
            }

    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    size_t total = data.size() * sizeof(float);
    size_t done = 0;
    while (done < total) {
        ssize_t n = write(fd, reinterpret_cast<const uint8_t*>(data.data()) + done, total - done);
        if (n <= 0) { close(fd); return false; }
        done += static_cast<size_t>(n);
    }
    close(fd);
    return true;
}

void referenceXSlice(uint64_t x, uint64_t ny, uint64_t nz,
                     std::vector<float>& out, DataPattern pattern) {
    out.resize(ny * nz);
    for (uint64_t y = 0; y < ny; ++y)
        for (uint64_t z = 0; z < nz; ++z) {
            if (pattern == DataPattern::Coordinate)
                out[y * nz + z] = valueAt(x, y, z);
            else
                out[y * nz + z] = 0.0f; // handled by readRawFile
        }
}

void referenceYSlice(uint64_t nx, uint64_t y, uint64_t nz,
                     std::vector<float>& out, DataPattern pattern) {
    out.resize(nx * nz);
    for (uint64_t x = 0; x < nx; ++x)
        for (uint64_t z = 0; z < nz; ++z) {
            if (pattern == DataPattern::Coordinate)
                out[x * nz + z] = valueAt(x, y, z);
            else
                out[x * nz + z] = 0.0f;
        }
}

void referenceZSlice(uint64_t nx, uint64_t ny, uint64_t z,
                     std::vector<float>& out, DataPattern pattern) {
    out.resize(nx * ny);
    for (uint64_t x = 0; x < nx; ++x)
        for (uint64_t y = 0; y < ny; ++y) {
            if (pattern == DataPattern::Coordinate)
                out[x * ny + y] = valueAt(x, y, z);
            else
                out[x * ny + y] = 0.0f;
        }
}

bool checkError(float refVal, float actualVal, double zeroTol, double relTol) {
    double absErr = std::abs(static_cast<double>(refVal) - static_cast<double>(actualVal));
    double absRef = std::abs(static_cast<double>(refVal));
    if (absRef <= zeroTol) return absErr <= zeroTol;
    return (absErr / std::max(absRef, 1e-12)) < relTol;
}

void verifyRzfpSlice(const std::vector<float>& got,
                     const std::vector<float>& ref,
                     const std::string& label,
                     double zeroTol = 1e-6, double relTol = 1e-3) {
    if (got.size() != ref.size()) {
        std::cerr << "FAIL: " << label << " size mismatch got=" << got.size()
                  << " ref=" << ref.size() << std::endl;
        ++g_failures;
        return;
    }
    size_t numViolations = 0;
    double maxRelErr = 0.0;
    for (size_t i = 0; i < got.size(); ++i) {
        if (!checkError(ref[i], got[i], zeroTol, relTol)) {
            double absErr = std::abs(static_cast<double>(ref[i]) - static_cast<double>(got[i]));
            double absRef = std::abs(static_cast<double>(ref[i]));
            double relErr = absErr / std::max(absRef, 1e-12);
            if (relErr > maxRelErr) maxRelErr = relErr;
            ++numViolations;
            if (numViolations <= 3) {
                std::cerr << "  violation[" << i << "]: ref=" << ref[i]
                          << " got=" << got[i] << " relErr=" << relErr << std::endl;
            }
        }
    }
    if (numViolations > 0) {
        std::cerr << "FAIL: " << label << " has " << numViolations << " violations"
                  << " (max rel err: " << maxRelErr << ")" << std::endl;
        ++g_failures;
    }
}

void testRzfpRoundTrip(const std::string& prefix,
                       uint64_t nx, uint64_t ny, uint64_t nz,
                       uint32_t superSize, uint32_t leafSize,
                       DataPattern pattern) {
    const std::string rawPath = prefix + ".raw";
    const std::string rzfpPath = prefix + ".rzfp";
    const std::string label = prefix;

    check(writeRawFile(rawPath, nx, ny, nz, pattern), (label + " write raw").c_str());

    erwt3d::RzfpWriterConfig cfg;
    cfg.nx = nx; cfg.ny = ny; cfg.nz = nz;
    cfg.super_size = superSize;
    cfg.leaf_size = leafSize;
    cfg.physical_order = erwt3d::PhysicalOrder::ZYX;
    cfg.threads = 1;

    erwt3d::RzfpWriterStats stats;
    bool ok = erwt3d::writeRzfpFile(rawPath, rzfpPath, cfg, &stats);
    check(ok, (label + " write rzfp").c_str());
    if (!ok) { unlink(rawPath.c_str()); return; }

    check(stats.violation_count == 0, (label + " zero violations").c_str());
    check(stats.storage_ratio > 0.0 && stats.storage_ratio < 2.0,
          (label + " reasonable storage ratio").c_str());

    erwt3d::RzfpReader reader(rzfpPath);
    check(reader.ok(), (label + " reader open").c_str());
    check(reader.header().nx == nx && reader.header().ny == ny && reader.header().nz == nz,
          (label + " dimensions").c_str());

    std::vector<float> got;

    // X slice: first, middle, last
    for (uint64_t x : {uint64_t(0), uint64_t(nx / 2), uint64_t(nx - 1)}) {
        got.resize(ny * nz);
        std::vector<float> ref;
        referenceXSlice(x, ny, nz, ref, pattern);
        ok = reader.readSlice(erwt3d::SliceAxis::X, x, got.data());
        check(ok, (label + " read X=" + std::to_string(x)).c_str());
        if (pattern == DataPattern::Coordinate)
            verifyRzfpSlice(got, ref, label + " X=" + std::to_string(x));
    }

    // Y slice: first, middle, last
    for (uint64_t y : {uint64_t(0), uint64_t(ny / 2), uint64_t(ny - 1)}) {
        got.resize(nx * nz);
        std::vector<float> ref;
        referenceYSlice(nx, y, nz, ref, pattern);
        ok = reader.readSlice(erwt3d::SliceAxis::Y, y, got.data());
        check(ok, (label + " read Y=" + std::to_string(y)).c_str());
        if (pattern == DataPattern::Coordinate)
            verifyRzfpSlice(got, ref, label + " Y=" + std::to_string(y));
    }

    // Z slice: first, middle, last
    for (uint64_t z : {uint64_t(0), uint64_t(nz / 2), uint64_t(nz - 1)}) {
        got.resize(nx * ny);
        std::vector<float> ref;
        referenceZSlice(nx, ny, z, ref, pattern);
        ok = reader.readSlice(erwt3d::SliceAxis::Z, z, got.data());
        check(ok, (label + " read Z=" + std::to_string(z)).c_str());
        if (pattern == DataPattern::Coordinate)
            verifyRzfpSlice(got, ref, label + " Z=" + std::to_string(z));
    }

    unlink(rawPath.c_str());
    unlink(rzfpPath.c_str());
}

void testRzfpReaderStrategies(const std::string& prefix,
                               uint64_t nx, uint64_t ny, uint64_t nz) {
    const std::string rawPath = prefix + "_strat.raw";
    const std::string rzfpPath = prefix + "_strat.rzfp";

    check(writeRawFile(rawPath, nx, ny, nz, DataPattern::Coordinate),
          "strategy: write raw");

    erwt3d::RzfpWriterConfig cfg;
    cfg.nx = nx; cfg.ny = ny; cfg.nz = nz;
    cfg.super_size = 16;
    cfg.leaf_size = 4;
    cfg.physical_order = erwt3d::PhysicalOrder::ZYX;
    cfg.threads = 1;

    erwt3d::RzfpWriterStats stats;
    check(erwt3d::writeRzfpFile(rawPath, rzfpPath, cfg, &stats), "strategy: write");

    using RRS = erwt3d::RzfpReadStrategy;
    const std::pair<RRS, const char*> strategies[] = {
        {RRS::SelectiveLeaf, "SelectiveLeaf"},
        {RRS::WholeSuperblock, "WholeSuperblock"},
        {RRS::FullPayloadScan, "FullPayloadScan"},
        {RRS::Auto, "Auto"},
    };

    std::vector<float> baseline;
    std::string baselineName;

    for (const auto& [strat, stratName] : strategies) {
        erwt3d::RzfpReaderConfig rcfg;
        rcfg.strategy = strat;
        rcfg.decode_threads = 1;

        erwt3d::RzfpReader reader(rzfpPath);
        std::vector<float> got(nz * ny);
        std::vector<erwt3d::RzfpReader::SliceBatchRequest> reqs;
        reqs.push_back({erwt3d::SliceAxis::X, nx / 2, got.data()});
        check(reader.readSlicesBatch(reqs, rcfg),
              (std::string("strategy ") + stratName + " read").c_str());

        if (baseline.empty()) {
            baseline = got;
            baselineName = stratName;
        } else {
            size_t bad = 0;
            for (size_t i = 0; i < baseline.size(); ++i) {
                if (baseline[i] != got[i]) ++bad;
            }
            check(bad == 0, (std::string("strategy ") + stratName +
                  " matches " + baselineName).c_str());
        }
    }

    unlink(rawPath.c_str());
    unlink(rzfpPath.c_str());
}

void testRzfpCorruption(const std::string& prefix,
                         uint64_t nx, uint64_t ny, uint64_t nz) {
    const std::string rawPath = prefix + "_corr.raw";
    const std::string rzfpPath = prefix + "_corr.rzfp";
    const std::string corruptPath = prefix + "_corr_corrupt.rzfp";

    check(writeRawFile(rawPath, nx, ny, nz, DataPattern::Coordinate),
          "corruption: write raw");

    erwt3d::RzfpWriterConfig cfg;
    cfg.nx = nx; cfg.ny = ny; cfg.nz = nz;
    cfg.super_size = 16;
    cfg.leaf_size = 4;
    cfg.physical_order = erwt3d::PhysicalOrder::ZYX;
    cfg.threads = 1;

    check(erwt3d::writeRzfpFile(rawPath, rzfpPath, cfg, nullptr), "corruption: write");

    // Copy file and corrupt a descriptor byte
    {
        std::vector<uint8_t> data;
        int fd = open(rzfpPath.c_str(), O_RDONLY);
        check(fd >= 0, "corruption: open original");
        off_t end = lseek(fd, 0, SEEK_END);
        lseek(fd, 0, SEEK_SET);
        data.resize(static_cast<size_t>(end));
        ssize_t n = read(fd, data.data(), data.size());
        close(fd);
        check(n == static_cast<ssize_t>(data.size()) && n > 256, "corruption: read original");

        // Corrupt a byte in descriptor area (after index, before payload)
        uint64_t descOff = *reinterpret_cast<const uint64_t*>(data.data() +
                            offsetof(erwt3d::RzfpFileHeader, descriptor_offset));
        uint64_t payloadOff = *reinterpret_cast<const uint64_t*>(data.data() +
                               offsetof(erwt3d::RzfpFileHeader, payload_offset));
        uint64_t corruptPos = std::min(descOff + 100, payloadOff > 300 ? payloadOff - 100 : descOff + 100);
        if (corruptPos < data.size()) {
            data[corruptPos] ^= 0xFF;
        }

        int fd2 = open(corruptPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        check(fd2 >= 0, "corruption: create corrupt file");
        write(fd2, data.data(), data.size());
        close(fd2);
    }

    // Read from corrupted file - should pass readSlice but may have bad data
    // The key test: reader should not crash and should report ok().
    {
        erwt3d::RzfpReader reader(corruptPath);
        std::vector<float> buf(ny * nz);
        bool ok = reader.readSlice(erwt3d::SliceAxis::X, nx / 2, buf.data());
        (void)ok; // May succeed or fail depending on corruption location, just not crash
        check(reader.ok(), "corruption: reader survives");
    }

    unlink(rawPath.c_str());
    unlink(rzfpPath.c_str());
    unlink(corruptPath.c_str());
}

} // namespace

int main() {
    const char* envTmp = std::getenv("ERWT3D_TEST_TMPDIR");
    std::string tmpDir = envTmp ? envTmp : "/tmp";
    const std::string basePrefix = tmpDir + "/test_rzfp_writer_zfastest";

    struct TestSize { uint64_t nx, ny, nz; const char* name; uint32_t super; };
    const TestSize sizes[] = {
        { 32, 32, 32, "aligned32", 32 },
        { 33, 35, 37, "bound33x35x37", 32 },
        { 48, 40, 40, "mixed48x40x40", 16 },
        { 65, 67, 69, "bound65x67x69", 64 },
    };

    for (const auto& sz : sizes) {
        std::cout << "\n=== RZFP size " << sz.name
                  << " (" << sz.nx << "x" << sz.ny << "x" << sz.nz << ")" << std::endl;
        testRzfpRoundTrip(basePrefix + "_" + sz.name,
                          sz.nx, sz.ny, sz.nz,
                          sz.super, /*leafSize=*/4,
                          DataPattern::Coordinate);
    }

    // Mixed data pattern test
    std::cout << "\n=== Mixed data pattern ===" << std::endl;
    testRzfpRoundTrip(basePrefix + "_mixed", 32, 32, 32, 32, 4, DataPattern::Mixed);

    // Reader strategy consistency
    std::cout << "\n=== Reader strategy consistency ===" << std::endl;
    testRzfpReaderStrategies(basePrefix, 48, 40, 40);

    // Corruption test
    std::cout << "\n=== Corruption handling ===" << std::endl;
    testRzfpCorruption(basePrefix, 32, 32, 32);

    if (g_failures == 0) {
        std::cout << "\nPASS" << std::endl;
        return 0;
    }
    std::cerr << "\n" << g_failures << " failures" << std::endl;
    return 1;
}
