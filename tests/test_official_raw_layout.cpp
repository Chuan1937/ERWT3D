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
#include <sstream>
#include <sys/stat.h>

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
    check(erwt3d::rawOffsetZFastest(0, 0, 1, ny, nz) ==
          erwt3d::rawOffsetZFastest(0, 0, 0, ny, nz) + 1, "z stride");
    check(erwt3d::rawOffsetZFastest(0, 1, 0, ny, nz) ==
          erwt3d::rawOffsetZFastest(0, 0, 0, ny, nz) + nz, "y stride");
    check(erwt3d::rawOffsetZFastest(1, 0, 0, ny, nz) ==
          erwt3d::rawOffsetZFastest(0, 0, 0, ny, nz) + ny * nz, "x stride");
}

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

void verifyAllSlices(erwt3d::ERWT3DReader& reader,
                     uint64_t nx, uint64_t ny, uint64_t nz,
                     const std::string& label) {
    std::vector<float> got;
    std::vector<float> ref;

    {
        uint64_t x = nx / 2;
        got.resize(ny * nz);
        ref.clear();
        referenceXSlice(x, ny, nz, ref);
        check(reader.readSlice(erwt3d::SliceAxis::X, x, got.data()),
              (label + " read X slice").c_str());
        check(slicesEqual(got, ref), (label + " X slice exact match").c_str());
    }

    {
        uint64_t y = ny / 2;
        got.resize(nx * nz);
        ref.clear();
        referenceYSlice(nx, y, nz, ref);
        check(reader.readSlice(erwt3d::SliceAxis::Y, y, got.data()),
              (label + " read Y slice").c_str());
        check(slicesEqual(got, ref), (label + " Y slice exact match").c_str());
    }

    {
        uint64_t z = nz / 2;
        got.resize(nx * ny);
        ref.clear();
        referenceZSlice(nx, ny, z, ref);
        check(reader.readSlice(erwt3d::SliceAxis::Z, z, got.data()),
              (label + " read Z slice").c_str());
        check(slicesEqual(got, ref), (label + " Z slice exact match").c_str());
    }
}

bool testWriterReaderRoundTrip(const std::string& prefix,
                                uint64_t nx, uint64_t ny, uint64_t nz,
                                bool fromFile, bool compress,
                                bool xpanel, uint32_t panelStride) {
    const std::string rawPath = prefix + ".raw";
    const std::string erwtPath = prefix + ".erwt3d";

    check(writeRawFile(rawPath, nx, ny, nz), "write raw file");

    uint32_t pa = xpanel ? 0u : 0u;
    uint32_t ps = xpanel ? panelStride : 0u;

    bool ok;
    if (fromFile) {
        ok = erwt3d::writeERWT3DFromFile(
            erwtPath, rawPath, nx, ny, nz,
            /*superX=*/4, /*superY=*/4, /*superZ=*/4,
            /*leafX=*/2, /*leafY=*/2, /*leafZ=*/2,
            /*numThreads=*/1, /*memoryLimitMB=*/256,
            pa, ps, compress);
    } else {
        std::vector<float> rawData(nx * ny * nz);
        check(readRawFile(rawPath, nx, ny, nz, rawData), "read raw data into memory");
        ok = erwt3d::writeERWT3D(
            erwtPath, rawData.data(), nx, ny, nz,
            /*superX=*/4, /*superY=*/4, /*superZ=*/4,
            /*leafX=*/2, /*leafY=*/2, /*leafZ=*/2,
            /*numThreads=*/1, /*memoryLimitMB=*/256,
            pa, ps);
    }
    check(ok, "write ERWT3D file");
    if (!ok) {
        unlink(rawPath.c_str());
        return false;
    }

    erwt3d::ERWT3DReader reader(erwtPath);
    check(reader.getHeader().nx == nx && reader.getHeader().ny == ny && reader.getHeader().nz == nz,
          "reader header dimensions");

    std::string label = prefix;
    verifyAllSlices(reader, nx, ny, nz, label);

    {
        std::vector<float> got(nx * ny * nz);
        check(reader.readFull(got.data()), (label + " read full volume").c_str());
        std::vector<float> original;
        check(readRawFile(rawPath, nx, ny, nz, original), (label + " read original raw for compare").c_str());
        check(slicesEqual(got, original), (label + " full restore exact match").c_str());
    }

    unlink(rawPath.c_str());
    unlink(erwtPath.c_str());
    return true;
}

std::string findPrecomputeXBinary() {
    const char* env = std::getenv("ERWT3D_PRECOMPUTE_X_BIN");
    if (env && env[0] != '\0') {
        struct stat st;
        if (stat(env, &st) == 0) return env;
    }

    std::vector<std::string> candidates;
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        std::string selfPath(buf);
        std::string selfDir = selfPath.substr(0, selfPath.find_last_of('/'));
        candidates.push_back(selfDir + "/../erwt3d_precompute_x");
        candidates.push_back(selfDir + "/erwt3d_precompute_x");
    }
    candidates.push_back("./erwt3d_precompute_x");
    candidates.push_back("../erwt3d_precompute_x");

    for (const auto& p : candidates) {
        struct stat st;
        if (stat(p.c_str(), &st) == 0 && (st.st_mode & S_IXUSR)) return p;
    }
    return "";
}

void testXPlaneSidecar(const std::string& prefix,
                       uint64_t nx, uint64_t ny, uint64_t nz,
                       uint32_t stride) {
    std::string precompBin = findPrecomputeXBinary();
    if (precompBin.empty()) {
        std::cout << "  (skipping sidecar test: erwt3d_precompute_x not found)" << std::endl;
        return;
    }

    const std::string rawPath = prefix + ".raw";
    const std::string erwtPath = prefix + ".erwt3d";
    const std::string xpPath = erwtPath + ".xp";

    check(writeRawFile(rawPath, nx, ny, nz), "sidecar: write raw file");

    bool ok = erwt3d::writeERWT3DFromFile(
        erwtPath, rawPath, nx, ny, nz,
        /*superX=*/4, /*superY=*/4, /*superZ=*/4,
        /*leafX=*/2, /*leafY=*/2, /*leafZ=*/2,
        /*numThreads=*/1, /*memoryLimitMB=*/256,
        /*panelAxis=*/0, /*panelStride=*/0,
        /*compress=*/false);
    check(ok, "sidecar: write ERWT3D file");

    // Build sidecar
    std::ostringstream cmd;
    cmd << precompBin
        << " --raw " << rawPath
        << " --erwt3d " << erwtPath
        << " --nx " << nx
        << " --ny " << ny
        << " --nz " << nz
        << " --mode sidecar"
        << " --stride " << stride
        << " --chunk-z-rows 64"
        << " --storage-budget 5.0"
        << " 2>&1";
    int rc = std::system(cmd.str().c_str());
    check(rc == 0, "sidecar: precompute_x sidecar mode");

    {
        erwt3d::ERWT3DReader reader(erwtPath);
        check(reader.getHeader().nx == nx, "sidecar: reader dimensions");

        std::string label = prefix + "_sidecar_s" + std::to_string(stride);
        verifyAllSlices(reader, nx, ny, nz, label);

        std::vector<float> got(nx * ny * nz);
        check(reader.readFull(got.data()), (label + " read full volume").c_str());
        std::vector<float> original;
        check(readRawFile(rawPath, nx, ny, nz, original), (label + " read original raw").c_str());
        check(slicesEqual(got, original), (label + " full restore exact match").c_str());
    }

    // Also test legacy X-plane mode
    std::ostringstream cmdLegacy;
    cmdLegacy << precompBin
              << " --raw " << rawPath
              << " --erwt3d " << erwtPath
              << " --nx " << nx
              << " --ny " << ny
              << " --nz " << nz
              << " --mode legacy"
              << " --stride " << stride
              << " 2>&1";
    rc = std::system(cmdLegacy.str().c_str());
    check(rc == 0, "sidecar: precompute_x legacy mode");

    {
        erwt3d::ERWT3DReader reader(erwtPath);
        std::string label = prefix + "_legacy_s" + std::to_string(stride);
        verifyAllSlices(reader, nx, ny, nz, label);

        std::vector<float> got(nx * ny * nz);
        check(reader.readFull(got.data()), (label + " read full volume").c_str());
        std::vector<float> original;
        check(readRawFile(rawPath, nx, ny, nz, original), (label + " read original raw").c_str());
        check(slicesEqual(got, original), (label + " full restore exact match").c_str());
    }

    unlink(xpPath.c_str());
    unlink(rawPath.c_str());
    unlink(erwtPath.c_str());
}

} // namespace

int main() {
    std::system("mkdir -p /mnt/g/erwt3d_tests");
    const std::string basePrefix = "/mnt/g/erwt3d_tests/test_official_raw_layout";

    struct TestSize { uint64_t nx, ny, nz; const char* name; };
    const TestSize sizes[] = {
        {  5,  7, 11, "tiny"},
        { 17, 19, 21, "small"},
        { 65, 67, 69, "medium"},
    };

    for (const auto& sz : sizes) {
        std::cout << "\n=== Size " << sz.name << " (" << sz.nx << "x" << sz.ny << "x" << sz.nz << ") ===" << std::endl;

        checkRawOffsetFormula(sz.nx, sz.ny, sz.nz);

        if (std::string(sz.name) != "medium") {
            // In-memory writer (fast for small sizes, slow for medium)
            testWriterReaderRoundTrip(basePrefix + "_" + sz.name + "_mem",
                                      sz.nx, sz.ny, sz.nz,
                                      /*fromFile=*/false, /*compress=*/false,
                                      /*xpanel=*/false, 0);
        }

        // File-based writer
        testWriterReaderRoundTrip(basePrefix + "_" + sz.name + "_file",
                                  sz.nx, sz.ny, sz.nz,
                                  /*fromFile=*/true, /*compress=*/false,
                                  /*xpanel=*/false, 0);

#ifdef ERWT3D_HAVE_LZ4
        // LZ4 compressed
        testWriterReaderRoundTrip(basePrefix + "_" + sz.name + "_lz4",
                                  sz.nx, sz.ny, sz.nz,
                                  /*fromFile=*/true, /*compress=*/true,
                                  /*xpanel=*/false, 0);
#endif

        // Sidecar tests (stride 1, 2, 3) for the smallest size only
        if (sz.nx == 5) {
#ifdef ERWT3D_HAVE_LZ4
            testXPlaneSidecar(basePrefix + "_" + sz.name, sz.nx, sz.ny, sz.nz, 1);
            testXPlaneSidecar(basePrefix + "_" + sz.name, sz.nx, sz.ny, sz.nz, 2);
            testXPlaneSidecar(basePrefix + "_" + sz.name, sz.nx, sz.ny, sz.nz, 3);
#endif
        }
    }

    if (g_failures == 0) {
        std::cout << "\nPASS" << std::endl;
        return 0;
    }
    std::cerr << "\n" << g_failures << " failures" << std::endl;
    return 1;
}
