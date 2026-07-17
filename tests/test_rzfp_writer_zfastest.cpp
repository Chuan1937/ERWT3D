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

float patternValue(uint64_t x, uint64_t y, uint64_t z, DataPattern pattern) {
    if (pattern == DataPattern::Coordinate)
        return valueAt(x, y, z);

    uint64_t h = (x * 73856093ULL + y * 19349663ULL + z * 83492791ULL) % 10;
    switch (h) {
        case 0:  return 0.0f;
        case 1:  return -0.0f;
        case 2:  return 1e-8f;
        case 3:  return -1e-8f;
        case 4:  return 0.5f;
        case 5:  return -3.14f;
        case 6:  return 1e6f;
        case 7:  return -1e6f;
        default: return valueAt(x % 3, y % 3, z % 3);
    }
}

bool writeRawFile(const std::string& path,
                  uint64_t nx, uint64_t ny, uint64_t nz,
                  DataPattern pattern) {
    std::vector<float> data(nx * ny * nz, 0.0f);
    for (uint64_t x = 0; x < nx; ++x)
        for (uint64_t y = 0; y < ny; ++y)
            for (uint64_t z = 0; z < nz; ++z)
                data[(x * ny + y) * nz + z] = patternValue(x, y, z, pattern);

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
        for (uint64_t z = 0; z < nz; ++z)
            out[y * nz + z] = patternValue(x, y, z, pattern);
}

void referenceYSlice(uint64_t nx, uint64_t y, uint64_t nz,
                     std::vector<float>& out, DataPattern pattern) {
    out.resize(nx * nz);
    for (uint64_t x = 0; x < nx; ++x)
        for (uint64_t z = 0; z < nz; ++z)
            out[x * nz + z] = patternValue(x, y, z, pattern);
}

void referenceZSlice(uint64_t nx, uint64_t ny, uint64_t z,
                     std::vector<float>& out, DataPattern pattern) {
    out.resize(nx * ny);
    for (uint64_t x = 0; x < nx; ++x)
        for (uint64_t y = 0; y < ny; ++y)
            out[x * ny + y] = patternValue(x, y, z, pattern);
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
        verifyRzfpSlice(got, ref, label + " X=" + std::to_string(x));
    }

    // Y slice: first, middle, last
    for (uint64_t y : {uint64_t(0), uint64_t(ny / 2), uint64_t(ny - 1)}) {
        got.resize(nx * nz);
        std::vector<float> ref;
        referenceYSlice(nx, y, nz, ref, pattern);
        ok = reader.readSlice(erwt3d::SliceAxis::Y, y, got.data());
        check(ok, (label + " read Y=" + std::to_string(y)).c_str());
        verifyRzfpSlice(got, ref, label + " Y=" + std::to_string(y));
    }

    // Z slice: first, middle, last
    for (uint64_t z : {uint64_t(0), uint64_t(nz / 2), uint64_t(nz - 1)}) {
        got.resize(nx * ny);
        std::vector<float> ref;
        referenceZSlice(nx, ny, z, ref, pattern);
        ok = reader.readSlice(erwt3d::SliceAxis::Z, z, got.data());
        check(ok, (label + " read Z=" + std::to_string(z)).c_str());
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
    const std::string corruptPath = prefix + "_corr_bad.rzfp";

    check(writeRawFile(rawPath, nx, ny, nz, DataPattern::Coordinate),
          "corruption: write raw");

    erwt3d::RzfpWriterConfig cfg;
    cfg.nx = nx; cfg.ny = ny; cfg.nz = nz;
    cfg.super_size = 16;
    cfg.leaf_size = 4;
    cfg.physical_order = erwt3d::PhysicalOrder::ZYX;
    cfg.threads = 1;

    check(erwt3d::writeRzfpFile(rawPath, rzfpPath, cfg, nullptr), "corruption: write");

    // Helper: copy file to new path with a byte modification
    auto makeCorrupt = [&](const std::string& outPath, uint64_t off, uint8_t newVal) -> bool {
        std::vector<uint8_t> data;
        int fd = open(rzfpPath.c_str(), O_RDONLY);
        if (fd < 0) return false;
        off_t end = lseek(fd, 0, SEEK_END);
        lseek(fd, 0, SEEK_SET);
        data.resize(static_cast<size_t>(end));
        if (read(fd, data.data(), data.size()) != static_cast<ssize_t>(data.size())) { close(fd); return false; }
        close(fd);
        if (off >= data.size()) return false;
        data[off] = newVal;
        int fd2 = open(outPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd2 < 0) return false;
        write(fd2, data.data(), data.size());
        close(fd2);
        return true;
    };

    // Test 1: Bad magic → reader should reject
    {
        unlink(corruptPath.c_str());
        check(makeCorrupt(corruptPath, 2, 0xFF), "corr: make bad magic");
        erwt3d::RzfpReader reader(corruptPath);
        check(!reader.ok(), "corr: bad magic rejected");
    }

    // Test 2: Corrupt payload_offset beyond file → reader should reject
    {
        unlink(corruptPath.c_str());
        uint64_t idxOff = sizeof(erwt3d::RzfpFileHeader);
        check(makeCorrupt(corruptPath, idxOff + 6, 0xFF), "corr: make bad index");
        erwt3d::RzfpReader reader(corruptPath);
        check(!reader.ok(), "corr: bad index rejected");
    }

    // Test 3: Corrupt descriptor codec value → reader should reject
    {
        unlink(corruptPath.c_str());
        erwt3d::RzfpReader readerOrig(rzfpPath);
        check(readerOrig.ok(), "corr: original is valid");
        uint64_t descOff = readerOrig.header().descriptor_offset + 1;
        check(makeCorrupt(corruptPath, descOff, 0xFF), "corr: make bad descriptor");
        erwt3d::RzfpReader readerBad(corruptPath);
        check(!readerBad.ok(), "corr: bad descriptor rejected");
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
