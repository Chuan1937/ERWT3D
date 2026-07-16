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

float expectedValue(uint64_t x, uint64_t y, uint64_t z) {
    return static_cast<float>(x * 1000000ULL + y * 1000ULL + z);
}

bool writeRawFile(const std::string& path,
                  uint64_t nx, uint64_t ny, uint64_t nz) {
    std::vector<float> data(nx * ny * nz);
    for (uint64_t x = 0; x < nx; ++x)
        for (uint64_t y = 0; y < ny; ++y)
            for (uint64_t z = 0; z < nz; ++z)
                data[(x * ny + y) * nz + z] = expectedValue(x, y, z);

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
                     std::vector<float>& out) {
    out.resize(ny * nz);
    for (uint64_t y = 0; y < ny; ++y)
        for (uint64_t z = 0; z < nz; ++z)
            out[y * nz + z] = expectedValue(x, y, z);
}

void referenceYSlice(uint64_t nx, uint64_t y, uint64_t nz,
                     std::vector<float>& out) {
    out.resize(nx * nz);
    for (uint64_t x = 0; x < nx; ++x)
        for (uint64_t z = 0; z < nz; ++z)
            out[x * nz + z] = expectedValue(x, y, z);
}

void referenceZSlice(uint64_t nx, uint64_t ny, uint64_t z,
                     std::vector<float>& out) {
    out.resize(nx * ny);
    for (uint64_t x = 0; x < nx; ++x)
        for (uint64_t y = 0; y < ny; ++y)
            out[x * ny + y] = expectedValue(x, y, z);
}

bool checkRelError(float refVal, float actualVal, double tol) {
    double absErr = std::abs(static_cast<double>(refVal) - static_cast<double>(actualVal));
    double absRef = std::abs(static_cast<double>(refVal));
    double relErr = absErr / std::max(absRef, 1e-12);
    return relErr < tol;
}

bool checkRelErrorZero(float refVal, float actualVal, double zeroTol) {
    double absErr = std::abs(static_cast<double>(refVal) - static_cast<double>(actualVal));
    double absRef = std::abs(static_cast<double>(refVal));
    if (absRef <= zeroTol) {
        return absErr <= zeroTol;
    }
    double relErr = absErr / std::max(absRef, 1e-12);
    return relErr < 1e-3;
}

void verifyRzfpSlice(const std::vector<float>& got,
                     const std::vector<float>& ref,
                     const std::string& label) {
    check(got.size() == ref.size(), (label + " size match").c_str());
    if (got.size() != ref.size()) return;

    size_t numViolations = 0;
    double maxRelErr = 0.0;
    uint64_t firstBadX = 0, firstBadY = 0;
    float firstBadGot = 0, firstBadRef = 0;

    for (size_t i = 0; i < got.size(); ++i) {
        if (!checkRelErrorZero(ref[i], got[i], 1e-6)) {
            if (numViolations == 0) {
                firstBadX = i;
                firstBadGot = got[i];
                firstBadRef = ref[i];
            }
            double absErr = std::abs(static_cast<double>(ref[i]) - static_cast<double>(got[i]));
            double absRef = std::abs(static_cast<double>(ref[i]));
            double relErr = absErr / std::max(absRef, 1e-12);
            if (relErr > maxRelErr) maxRelErr = relErr;
            ++numViolations;
        }
    }

    if (numViolations > 0) {
        std::cerr << "FAIL: " << label << " has " << numViolations << " violations"
                  << " (max rel err: " << maxRelErr
                  << ", first bad idx=" << firstBadX
                  << " got=" << firstBadGot << " ref=" << firstBadRef << ")" << std::endl;
        ++g_failures;
    }
}

void testRzfpRoundTrip(const std::string& prefix,
                       uint64_t nx, uint64_t ny, uint64_t nz,
                       uint32_t superSize, uint32_t leafSize) {
    const std::string rawPath = prefix + ".raw";
    const std::string rzfpPath = prefix + ".rzfp";

    check(writeRawFile(rawPath, nx, ny, nz), "rzfp: write raw file");

    erwt3d::RzfpWriterConfig cfg;
    cfg.nx = nx;
    cfg.ny = ny;
    cfg.nz = nz;
    cfg.super_size = superSize;
    cfg.leaf_size = leafSize;
    cfg.physical_order = erwt3d::PhysicalOrder::ZYX;
    cfg.threads = 1;

    erwt3d::RzfpWriterStats stats;
    bool ok = erwt3d::writeRzfpFile(rawPath, rzfpPath, cfg, &stats);
    check(ok, "rzfp: write file");
    if (!ok) { unlink(rawPath.c_str()); return; }

    check(stats.violation_count == 0, "rzfp: zero violations during write");

    erwt3d::RzfpReader reader(rzfpPath);
    check(reader.ok(), "rzfp: reader open");
    check(reader.header().nx == nx && reader.header().ny == ny && reader.header().nz == nz,
          "rzfp: reader dimensions");

    std::vector<float> got;
    std::vector<float> ref;

    // X slice
    {
        uint64_t x = nx / 2;
        got.resize(ny * nz);
        referenceXSlice(x, ny, nz, ref);
        ok = reader.readSlice(erwt3d::SliceAxis::X, x, got.data());
        check(ok, "rzfp: read X slice");
        verifyRzfpSlice(got, ref, prefix + " X slice");
    }

    // Y slice
    {
        uint64_t y = ny / 2;
        got.resize(nx * nz);
        referenceYSlice(nx, y, nz, ref);
        ok = reader.readSlice(erwt3d::SliceAxis::Y, y, got.data());
        check(ok, "rzfp: read Y slice");
        verifyRzfpSlice(got, ref, prefix + " Y slice");
    }

    // Z slice
    {
        uint64_t z = nz / 2;
        got.resize(nx * ny);
        referenceZSlice(nx, ny, z, ref);
        ok = reader.readSlice(erwt3d::SliceAxis::Z, z, got.data());
        check(ok, "rzfp: read Z slice");
        verifyRzfpSlice(got, ref, prefix + " Z slice");
    }

    unlink(rawPath.c_str());
    unlink(rzfpPath.c_str());
}

} // namespace

int main() {
    std::system("mkdir -p /mnt/g/erwt3d_tests");
    const std::string basePrefix = "/mnt/g/erwt3d_tests/test_rzfp_writer_zfastest";

    struct TestSize { uint64_t nx, ny, nz; const char* name; uint32_t super; };
    const TestSize sizes[] = {
        { 32, 32, 32, "s32", 32 },
        { 48, 40, 40, "s48x40x40", 16 },
    };

    for (const auto& sz : sizes) {
        std::cout << "\n=== RZFP size " << sz.name
                  << " (" << sz.nx << "x" << sz.ny << "x" << sz.nz << ") ===" << std::endl;
        testRzfpRoundTrip(basePrefix + "_" + sz.name,
                          sz.nx, sz.ny, sz.nz,
                          /*superSize=*/sz.super, /*leafSize=*/4);
    }

    if (g_failures == 0) {
        std::cout << "\nPASS" << std::endl;
        return 0;
    }
    std::cerr << "\n" << g_failures << " failures" << std::endl;
    return 1;
}
