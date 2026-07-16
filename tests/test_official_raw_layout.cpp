#include "erwt3d/raw_layout.hpp"
#include "erwt3d/reader.hpp"
#include "erwt3d/writer.hpp"

#include <cstdint>
#include <cstdlib>
#include <cmath>
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
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    for (uint64_t x = 0; x < nx; ++x) {
        for (uint64_t y = 0; y < ny; ++y) {
            for (uint64_t z = 0; z < nz; ++z) {
                float v = expectedValue(x, y, z);
                if (write(fd, &v, sizeof(float)) != sizeof(float)) {
                    close(fd);
                    return false;
                }
            }
        }
    }
    close(fd);
    return true;
}

bool readRawFile(const std::string& path,
                 uint64_t nx, uint64_t ny, uint64_t nz,
                 std::vector<float>& out) {
    out.resize(nx * ny * nz);
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    size_t total = out.size() * sizeof(float);
    size_t done = 0;
    while (done < total) {
        ssize_t n = read(fd, reinterpret_cast<uint8_t*>(out.data()) + done, total - done);
        if (n <= 0) { close(fd); return false; }
        done += static_cast<size_t>(n);
    }
    close(fd);
    return true;
}

void checkRawOffsetFormula(uint64_t nx, uint64_t ny, uint64_t nz) {
    (void)nx;
    for (uint64_t x = 0; x < 3; ++x) {
        for (uint64_t y = 0; y < 3; ++y) {
            for (uint64_t z = 0; z < 3; ++z) {
                uint64_t off = erwt3d::rawOffsetZFastest(x, y, z, ny, nz);
                check(off == (x * ny + y) * nz + z, "rawOffsetZFastest formula");
            }
        }
    }
    // Check stride continuity.
    check(erwt3d::rawOffsetZFastest(0, 0, 1, ny, nz) ==
          erwt3d::rawOffsetZFastest(0, 0, 0, ny, nz) + 1, "z stride");
    check(erwt3d::rawOffsetZFastest(0, 1, 0, ny, nz) ==
          erwt3d::rawOffsetZFastest(0, 0, 0, ny, nz) + nz, "y stride");
    check(erwt3d::rawOffsetZFastest(1, 0, 0, ny, nz) ==
          erwt3d::rawOffsetZFastest(0, 0, 0, ny, nz) + ny * nz, "x stride");
}

// Reference slice layouts follow the official Z-fastest raw ordering:
// raw offset(x,y,z) = (x*ny + y)*nz + z.
// X slice (fixed x): output[y * nz + z]
// Y slice (fixed y): output[x * nz + z]
// Z slice (fixed z): output[x * ny + y]
void referenceXSlice(uint64_t x, uint64_t ny, uint64_t nz,
                     std::vector<float>& out) {
    out.resize(ny * nz);
    for (uint64_t y = 0; y < ny; ++y) {
        for (uint64_t z = 0; z < nz; ++z) {
            out[y * nz + z] = expectedValue(x, y, z);
        }
    }
}

void referenceYSlice(uint64_t nx, uint64_t y, uint64_t nz,
                     std::vector<float>& out) {
    out.resize(nx * nz);
    for (uint64_t x = 0; x < nx; ++x) {
        for (uint64_t z = 0; z < nz; ++z) {
            out[x * nz + z] = expectedValue(x, y, z);
        }
    }
}

void referenceZSlice(uint64_t nx, uint64_t ny, uint64_t z,
                     std::vector<float>& out) {
    out.resize(nx * ny);
    for (uint64_t x = 0; x < nx; ++x) {
        for (uint64_t y = 0; y < ny; ++y) {
            out[x * ny + y] = expectedValue(x, y, z);
        }
    }
}

bool slicesEqual(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::isnan(a[i]) || std::isnan(b[i])) return false;
        if (a[i] != b[i]) return false;
    }
    return true;
}

void testWriterReaderRoundTrip(const std::string& prefix,
                               uint64_t nx, uint64_t ny, uint64_t nz,
                               bool fromFile, bool compress) {
    const std::string rawPath = prefix + ".raw";
    const std::string erwtPath = prefix + ".erwt3d";

    check(writeRawFile(rawPath, nx, ny, nz), "write raw file");

    bool ok;
    if (fromFile) {
        ok = erwt3d::writeERWT3DFromFile(
            erwtPath, rawPath, nx, ny, nz,
            /*superX=*/4, /*superY=*/4, /*superZ=*/4,
            /*leafX=*/2, /*leafY=*/2, /*leafZ=*/2,
            /*numThreads=*/1, /*memoryLimitMB=*/256,
            /*panelAxis=*/0, /*panelStride=*/0,
            compress);
    } else {
        std::vector<float> rawData(nx * ny * nz);
        check(readRawFile(rawPath, nx, ny, nz, rawData), "read raw data into memory");
        ok = erwt3d::writeERWT3D(
            erwtPath, rawData.data(), nx, ny, nz,
            /*superX=*/4, /*superY=*/4, /*superZ=*/4,
            /*leafX=*/2, /*leafY=*/2, /*leafZ=*/2,
            /*numThreads=*/1, /*memoryLimitMB=*/256,
            /*panelAxis=*/0, /*panelStride=*/0);
    }
    check(ok, "write ERWT3D file");

    erwt3d::ERWT3DReader reader(erwtPath);
    check(reader.getHeader().nx == nx && reader.getHeader().ny == ny && reader.getHeader().nz == nz,
          "reader header dimensions");

    std::vector<float> got;
    std::vector<float> ref;

    // X slice at x = nx/2
    {
        uint64_t x = nx / 2;
        got.resize(ny * nz);
        ref.clear();
        referenceXSlice(x, ny, nz, ref);
        check(reader.readSlice(erwt3d::SliceAxis::X, x, got.data()),
              "read X slice");
        check(slicesEqual(got, ref), "X slice exact match");
    }

    // Y slice at y = ny/2
    {
        uint64_t y = ny / 2;
        got.resize(nx * nz);
        ref.clear();
        referenceYSlice(nx, y, nz, ref);
        check(reader.readSlice(erwt3d::SliceAxis::Y, y, got.data()),
              "read Y slice");
        check(slicesEqual(got, ref), "Y slice exact match");
    }

    // Z slice at z = nz/2
    {
        uint64_t z = nz / 2;
        got.resize(nx * ny);
        ref.clear();
        referenceZSlice(nx, ny, z, ref);
        check(reader.readSlice(erwt3d::SliceAxis::Z, z, got.data()),
              "read Z slice");
        check(slicesEqual(got, ref), "Z slice exact match");
    }

    // Full restore must match the original raw file byte-for-byte.
    {
        got.resize(nx * ny * nz);
        check(reader.readFull(got.data()), "read full volume");
        std::vector<float> original;
        check(readRawFile(rawPath, nx, ny, nz, original), "read original raw for compare");
        check(slicesEqual(got, original), "full restore exact match");
    }

    unlink(rawPath.c_str());
    unlink(erwtPath.c_str());
}

} // namespace

int main() {
    std::system("mkdir -p /mnt/d/opencode_tests");
    const std::string prefix = "/mnt/d/opencode_tests/test_official_raw_layout";

    const uint64_t nx = 5;
    const uint64_t ny = 7;
    const uint64_t nz = 11;

    checkRawOffsetFormula(nx, ny, nz);

    // Lossless in-memory writer.
    testWriterReaderRoundTrip(prefix + "_mem", nx, ny, nz,
                              /*fromFile=*/false, /*compress=*/false);

    // Lossless file-based writer.
    testWriterReaderRoundTrip(prefix + "_file", nx, ny, nz,
                              /*fromFile=*/true, /*compress=*/false);

#ifdef ERWT3D_HAVE_LZ4
    // Compressed file-based writer.
    testWriterReaderRoundTrip(prefix + "_lz4", nx, ny, nz,
                              /*fromFile=*/true, /*compress=*/true);
#endif

    if (g_failures == 0) {
        std::cout << "PASS" << std::endl;
        return 0;
    }
    std::cerr << g_failures << " failures" << std::endl;
    return 1;
}
