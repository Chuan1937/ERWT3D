#include "erwt3d/writer.hpp"
#include "erwt3d/reader.hpp"
#include "erwt3d/raw_x_aux.hpp"
#include "erwt3d/raw_layout.hpp"
#include "erwt3d/rzfp_writer.hpp"
#include "erwt3d/rzfp_reader.hpp"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <random>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

struct TestData {
    std::vector<float> raw;
    uint64_t nx, ny, nz;
};

TestData generateData(uint64_t nx, uint64_t ny, uint64_t nz) {
    TestData td;
    td.nx = nx; td.ny = ny; td.nz = nz;
    td.raw.resize(nx * ny * nz);
    for (uint64_t x = 0; x < nx; ++x)
        for (uint64_t y = 0; y < ny; ++y)
            for (uint64_t z = 0; z < nz; ++z)
                td.raw[erwt3d::rawOffsetZFastest(x, y, z, ny, nz)] =
                    static_cast<float>(x * 100000 + y * 100 + z);
    return td;
}

void writeRaw(const TestData& td, const std::string& path) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    erwt3d::writeFullyAt(fd, td.raw.data(), td.raw.size() * sizeof(float), 0);
    fsync(fd);
    close(fd);
}

bool readXSlice(erwt3d::ERWT3DReader& reader, uint64_t x, std::vector<float>& out) {
    out.resize(reader.getHeader().ny * reader.getHeader().nz);
    return reader.readSlice(erwt3d::SliceAxis::X, x, out.data(), 1, 2048);
}

bool bitExact(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return false;
    return std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

std::vector<float> expectedXSlice(const TestData& td, uint64_t x) {
    std::vector<float> out(td.ny * td.nz);
    uint64_t off = erwt3d::rawOffsetZFastest(x, 0, 0, td.ny, td.nz);
    std::memcpy(out.data(), td.raw.data() + off, out.size() * sizeof(float));
    return out;
}

#define TEST(name) \
    do { std::cout << "  " << name << "... "; } while(0)
#define PASS() \
    do { std::cout << "PASS" << std::endl; } while(0)
#define FAIL(msg) \
    do { std::cerr << "FAIL: " << msg << std::endl; ++failures; return; } while(0)

int failures = 0;

// --- Dataset A: 17x19x23 ---

void testDatasetA_LZ4() {
    TEST("Dataset A (17x19x23) LZ4 + raw X aux");
    const auto td = generateData(17, 19, 23);
    std::string rawPath = "/tmp/test_raw_x_aux_a.raw";
    std::string erwt3dPath = "/tmp/test_raw_x_aux_a.erwt3d";
    writeRaw(td, rawPath);

    erwt3d::RawXAuxStats auxStats;
    bool ok = erwt3d::writeERWT3DFromFile(erwt3dPath, rawPath, td.nx, td.ny, td.nz,
                                           64, 64, 64, 4, 4, 4,
                                           4, 2048, 0, 0, false,
                                           erwt3d::RawXAuxMode::On,
                                           true, &auxStats);
    if (!ok || !auxStats.stored()) FAIL("write failed");

    erwt3d::ERWT3DReader reader(erwt3dPath);
    std::vector<float> actual;

    // Test key planes
    for (uint64_t x : {0ULL, 1ULL, 8ULL, 15ULL, 16ULL}) {
        if (!readXSlice(reader, x, actual)) FAIL("read X=" + std::to_string(x));
        auto expected = expectedXSlice(td, x);
        if (!bitExact(actual, expected)) FAIL("mismatch X=" + std::to_string(x));
    }

    // Test unordered batch with duplicates
    std::vector<uint64_t> xs = {16, 0, 3, 3, 8, 1};
    std::vector<std::vector<float>> bufs(xs.size(), std::vector<float>(td.ny * td.nz));
    std::vector<erwt3d::ERWT3DReader::SliceBatchRequest> reqs;
    for (size_t i = 0; i < xs.size(); ++i)
        reqs.push_back({erwt3d::SliceAxis::X, xs[i], bufs[i].data()});
    erwt3d::HDDReadWindowConfig wcfg;
    if (!reader.readSlicesBatch(reqs, 1, 2048, wcfg)) FAIL("batch read failed");
    for (size_t i = 0; i < xs.size(); ++i) {
        auto expected = expectedXSlice(td, xs[i]);
        if (!bitExact(bufs[i], expected)) FAIL("batch mismatch X=" + std::to_string(xs[i]));
    }

    unlink(rawPath.c_str()); unlink(erwt3dPath.c_str());
    PASS();
}

// --- Dataset B: 65x66x67 ---

void testDatasetB() {
    TEST("Dataset B (65x66x67) cross-SB + LZ4 compress");
    const auto td = generateData(65, 66, 67);
    std::string rawPath = "/tmp/test_raw_x_aux_b.raw";
    std::string erwt3dPath = "/tmp/test_raw_x_aux_b.erwt3d";
    writeRaw(td, rawPath);

    erwt3d::RawXAuxStats auxStats;
    bool ok = erwt3d::writeERWT3DFromFile(erwt3dPath, rawPath, td.nx, td.ny, td.nz,
                                           64, 64, 64, 4, 4, 4,
                                           4, 2048, 0, 0, true,
                                           erwt3d::RawXAuxMode::On,
                                           true, &auxStats);
    if (!ok || !auxStats.stored()) FAIL("write failed");

    erwt3d::ERWT3DReader reader(erwt3dPath);
    std::vector<float> actual;

    for (uint64_t x : {0ULL, 1ULL, 32ULL, 63ULL, 64ULL}) {
        if (!readXSlice(reader, x, actual)) FAIL("read X=" + std::to_string(x));
        auto expected = expectedXSlice(td, x);
        if (!bitExact(actual, expected)) FAIL("mismatch X=" + std::to_string(x));
    }

    // Batch continuous
    std::vector<uint64_t> xs = {10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
    std::vector<std::vector<float>> bufs2(xs.size(), std::vector<float>(td.ny * td.nz));
    std::vector<erwt3d::ERWT3DReader::SliceBatchRequest> reqs2;
    for (size_t i = 0; i < xs.size(); ++i)
        reqs2.push_back({erwt3d::SliceAxis::X, xs[i], bufs2[i].data()});
    erwt3d::HDDReadWindowConfig wcfg2;
    reader.readSlicesBatch(reqs2, 4, 2048, wcfg2);
    for (size_t i = 0; i < xs.size(); ++i) {
        auto expected = expectedXSlice(td, xs[i]);
        if (!bitExact(bufs2[i], expected)) FAIL("batch mismatch X=" + std::to_string(xs[i]));
    }

    // Verify Y/Z still readable
    std::vector<float> ySlice(td.nx * td.nz);
    reader.readSlice(erwt3d::SliceAxis::Y, 0, ySlice.data(), 1, 2048);
    size_t yErrors = 0;
    for (uint64_t x = 0; x < td.nx; ++x)
        for (uint64_t z = 0; z < td.nz; ++z)
            if (std::abs(ySlice[x * td.nz + z] -
                         static_cast<float>(x * 100000 + 0 * 100 + z)) > 1e-6f)
                ++yErrors;
    if (yErrors > 0) FAIL("Y slice has " + std::to_string(yErrors) + " errors");

    unlink(rawPath.c_str()); unlink(erwt3dPath.c_str());
    PASS();
}

// --- Mode tests: auto/on/off ---

void testModes() {
    TEST("Mode tests: on/auto/off");
    const auto td = generateData(17, 19, 23);
    std::string rawPath = "/tmp/test_raw_x_aux_m.raw";
    writeRaw(td, rawPath);

    // Off
    {
        std::string path = "/tmp/test_raw_x_aux_m_off.erwt3d";
        erwt3d::RawXAuxStats s;
        erwt3d::writeERWT3DFromFile(path, rawPath, td.nx, td.ny, td.nz,
                                     64, 64, 64, 4, 4, 4,
                                     4, 2048, 0, 0, false,
                                     erwt3d::RawXAuxMode::Off, true, &s);
        if (s.status != erwt3d::RawXAuxStatus::Disabled) FAIL("Off mode should Disable");
        unlink(path.c_str());
    }

    // On (force edge for small dataset)
    {
        std::string path = "/tmp/test_raw_x_aux_m_on.erwt3d";
        erwt3d::RawXAuxStats s;
        erwt3d::writeERWT3DFromFile(path, rawPath, td.nx, td.ny, td.nz,
                                     64, 64, 64, 4, 4, 4,
                                     4, 2048, 0, 0, false,
                                     erwt3d::RawXAuxMode::On, true, &s);
        if (!s.stored()) FAIL("On mode should store, got " + s.message);
        unlink(path.c_str());
    }

    // Auto with small data (will skip due to budget)
    {
        std::string path = "/tmp/test_raw_x_aux_m_auto.erwt3d";
        erwt3d::RawXAuxStats s;
        erwt3d::writeERWT3DFromFile(path, rawPath, td.nx, td.ny, td.nz,
                                     64, 64, 64, 4, 4, 4,
                                     4, 2048, 0, 0, false,
                                     erwt3d::RawXAuxMode::Auto, false, &s);
        if (s.status == erwt3d::RawXAuxStatus::Failed && s.stored())
            FAIL("Auto mode should not crash on budget skip");
        unlink(path.c_str());
    }

    unlink(rawPath.c_str());
    PASS();
}

// --- Storage hard limit test (only meaningful for large data) ---

void testStorageHardLimit() {
    TEST("Storage hard limit skipped for small test data");
    const auto td = generateData(17, 19, 23);
    std::string rawPath = "/tmp/test_raw_x_aux_h.raw";
    std::string path = "/tmp/test_raw_x_aux_h.erwt3d";
    writeRaw(td, rawPath);

    erwt3d::writeERWT3DFromFile(path, rawPath, td.nx, td.ny, td.nz,
                                 64, 64, 64, 4, 4, 4,
                                 4, 2048, 0, 0, false,
                                 erwt3d::RawXAuxMode::Off);

    // For small data (<10MB raw), ratio checks are skipped — overhead dominates
    // and the ratio is meaningless. For large data (≥10MB), the hard limit is enforced.
    // Since our test data is <10MB, this operation succeeds.
    erwt3d::RawXAuxStats s;
    bool ok = erwt3d::appendRawXAuxToFile(path, rawPath, td.nx, td.ny, td.nz, &s, true);
    if (!ok || !s.stored())
        FAIL("Small data should skip ratio check and store");

    unlink(rawPath.c_str()); unlink(path.c_str());
    PASS();
}

// --- Corrupt metadata fallback ---

void testCorruptMetadataFallback() {
    TEST("Corrupt raw X aux metadata fallback");
    const auto td = generateData(17, 19, 23);
    std::string rawPath = "/tmp/test_raw_x_aux_c.raw";
    std::string path = "/tmp/test_raw_x_aux_c.erwt3d";
    writeRaw(td, rawPath);

    erwt3d::writeERWT3DFromFile(path, rawPath, td.nx, td.ny, td.nz,
                                 64, 64, 64, 4, 4, 4,
                                 4, 2048, 0, 0, false,
                                 erwt3d::RawXAuxMode::On, true);

    // Corrupt version
    {
        int fd = open(path.c_str(), O_RDWR);
        erwt3d::ERWT3DHeader hdr;
        erwt3d::readFullyAt(fd, &hdr, sizeof(hdr), 0);
        hdr.reserved[10] = 99; // invalid version
        erwt3d::writeFullyAt(fd, &hdr, sizeof(hdr), 0);
        fsync(fd); close(fd);
    }

    erwt3d::ERWT3DReader reader(path);
    std::vector<float> actual;
    if (!readXSlice(reader, 0, actual)) FAIL("Should still read via main path");
    auto expected = expectedXSlice(td, 0);
    if (!bitExact(actual, expected)) FAIL("Corrupt aux metadata should fall back to main reader");

    unlink(rawPath.c_str()); unlink(path.c_str());
    PASS();
}

// --- Reader still works when raw X aux is missing ---

void testReaderWithoutRawXAux() {
    TEST("Reader without raw X aux still works");
    const auto td = generateData(17, 19, 23);
    std::string rawPath = "/tmp/test_raw_x_aux_nx.raw";
    std::string path = "/tmp/test_raw_x_aux_nx.erwt3d";
    writeRaw(td, rawPath);

    erwt3d::writeERWT3DFromFile(path, rawPath, td.nx, td.ny, td.nz,
                                 64, 64, 64, 4, 4, 4,
                                 4, 2048, 0, 0, false,
                                 erwt3d::RawXAuxMode::Off);

    erwt3d::ERWT3DReader reader(path);
    std::vector<float> actual;
    if (!readXSlice(reader, 0, actual)) FAIL("Read failed without raw X aux");
    auto expected = expectedXSlice(td, 0);
    if (!bitExact(actual, expected)) FAIL("Mismatch without raw X aux");

    unlink(rawPath.c_str()); unlink(path.c_str());
    PASS();
}

// --- RZFP Raw X Aux test ---

void testRzfpRawXAux() {
    TEST("RZFP Raw X Aux (17x19x23)");
    const auto td = generateData(17, 19, 23);
    std::string rawPath = "/tmp/test_raw_x_aux_rzfp.raw";
    std::string rzfpPath = "/tmp/test_raw_x_aux_rzfp.rzfp";
    writeRaw(td, rawPath);

    erwt3d::RzfpWriterConfig cfg;
    cfg.nx = 17; cfg.ny = 19; cfg.nz = 23;
    cfg.threads = 4;
    if (!erwt3d::writeRzfpFile(rawPath, rzfpPath, cfg)) FAIL("RZFP write failed");

    erwt3d::RawXAuxStats auxStats;
    if (!erwt3d::appendRawXAuxToRzfpFile(rzfpPath, rawPath, td.nx, td.ny, td.nz, &auxStats, true))
        FAIL("RZFP raw X aux append failed");
    if (!auxStats.stored()) FAIL("RZFP raw X aux should be stored");

    erwt3d::RzfpReader reader(rzfpPath);
    std::vector<float> actual(td.ny * td.nz);
    erwt3d::RzfpReaderConfig rcfg;
    for (uint64_t x : {0ULL, 8ULL, 16ULL}) {
        if (!reader.readSlice(erwt3d::SliceAxis::X, x, actual.data(), 1, 2048))
            FAIL("RZFP read X=" + std::to_string(x));
        auto expected = expectedXSlice(td, x);
        if (!bitExact(actual, expected))
            FAIL("RZFP X=" + std::to_string(x) + " mismatch");
    }

    // Y/Z fallback still works
    std::vector<float> ySlice(td.nx * td.nz);
    if (!reader.readSlice(erwt3d::SliceAxis::Y, 0, ySlice.data(), 1, 2048))
        FAIL("RZFP Y read failed");

    unlink(rawPath.c_str()); unlink(rzfpPath.c_str());
    PASS();
}

// --- Real hard limit test (data >= 10MB raw to enforce checks) ---

void testRealHardLimit() {
    TEST("Real hard limit (>1.50x, data >= 10MB)");
    // 256x256x64 = 4,194,304 floats * 4 = 16,777,216 bytes = 16MB
    const auto td = generateData(256, 256, 64);
    std::string rawPath = "/tmp/test_raw_x_aux_hardlimit.raw";
    std::string path = "/tmp/test_raw_x_aux_hardlimit.erwt3d";
    writeRaw(td, rawPath);

    // Write uncompressed main file (uncompressed = storage ratio ~1.0 main + 1.0 aux = ~2.0 > 1.45)
    erwt3d::RawXAuxStats stats;
    bool ok = erwt3d::writeERWT3DFromFile(path, rawPath, td.nx, td.ny, td.nz,
                                           64, 64, 64, 4, 4, 4,
                                           1, 2048, 0, 0, false,
                                           erwt3d::RawXAuxMode::On, true, &stats);
    if (ok) FAIL("Should reject >1.50x even with force-storage-edge on uncompressed");
    if (stats.status != erwt3d::RawXAuxStatus::SkippedStorageBudget)
        FAIL("Status should be SkippedStorageBudget, got " + std::to_string(static_cast<int>(stats.status)));
    if (stats.stored()) FAIL("Should not store");

    unlink(rawPath.c_str()); unlink(path.c_str());
    PASS();
}

// --- Transaction rollback test: simulate partial append ---

void testTransactionRollback() {
    TEST("Transaction rollback (partial append recovery)");
    const auto td = generateData(17, 19, 23);
    std::string rawPath = "/tmp/test_raw_x_aux_rollback.raw";
    std::string path = "/tmp/test_raw_x_aux_rollback.erwt3d";
    writeRaw(td, rawPath);

    erwt3d::writeERWT3DFromFile(path, rawPath, td.nx, td.ny, td.nz,
                                 64, 64, 64, 4, 4, 4,
                                 1, 2048, 0, 0, false,
                                 erwt3d::RawXAuxMode::Off);

    struct stat st0;
    stat(path.c_str(), &st0);
    uint64_t origSize = static_cast<uint64_t>(st0.st_size);

    // Read original header
    int fd = open(path.c_str(), O_RDWR);
    erwt3d::ERWT3DHeader origHdr;
    erwt3d::readFullyAt(fd, &origHdr, sizeof(origHdr), 0);

    // Simulate partial append: write garbage after the file, truncate to larger
    uint64_t fakeEnd = (origSize + 4095) & ~4095ULL;
    ftruncate(fd, static_cast<off_t>(fakeEnd + 1024));
    fsync(fd);

    // Write a fake flag to simulate a header that claims raw X aux exists but data is garbage
    origHdr.flags |= (1ULL << 7); // FLAG_HAS_RAW_X_AUX
    origHdr.reserved[7] = fakeEnd;
    origHdr.reserved[8] = 1024;
    origHdr.reserved[9] = td.ny * td.nz * 4;
    origHdr.reserved[10] = 1;
    erwt3d::writeFullyAt(fd, &origHdr, sizeof(origHdr), 0);
    fsync(fd);
    close(fd);

    // Reader should handle this: validation should fail (data is garbage, not raw X aux)
    // and fall back to main file reader
    erwt3d::ERWT3DReader reader(path);
    std::vector<float> actual;
    if (!readXSlice(reader, 0, actual)) FAIL("Should read via main path after corrupt raw X aux");

    // Now properly append raw X aux
    fd = open(path.c_str(), O_RDWR);
    ftruncate(fd, static_cast<off_t>(origSize));
    fsync(fd);
    erwt3d::writeFullyAt(fd, &origHdr, sizeof(origHdr), 0); // restore original header
    erwt3d::ERWT3DHeader cleanHdr;
    cleanHdr = origHdr;
    cleanHdr.flags &= ~(1ULL << 7); // clear raw X aux flag
    erwt3d::writeFullyAt(fd, &cleanHdr, sizeof(cleanHdr), 0);
    fsync(fd);
    close(fd);

    erwt3d::RawXAuxStats auxStats;
    if (!erwt3d::appendRawXAuxToFile(path, rawPath, td.nx, td.ny, td.nz, &auxStats, true))
        FAIL("Re-append should succeed");
    if (!auxStats.stored()) FAIL("Re-append should store");

    // Re-open and verify
    erwt3d::ERWT3DReader reader2(path);
    std::vector<float> actual2;
    if (!readXSlice(reader2, 0, actual2)) FAIL("Read after re-append");
    auto expected = expectedXSlice(td, 0);
    if (!bitExact(actual2, expected)) FAIL("Mismatch after re-append");

    unlink(rawPath.c_str()); unlink(path.c_str());
    PASS();
}

} // namespace

int main() {
    std::cout << "test_raw_x_aux" << std::endl;

    testDatasetA_LZ4();
    testDatasetB();
    testModes();
    testStorageHardLimit();
    testCorruptMetadataFallback();
    testReaderWithoutRawXAux();
    testRzfpRawXAux();
    testRealHardLimit();
    testTransactionRollback();

    std::cout << "\n" << (failures == 0 ? "ALL TESTS PASSED" : "FAILURES: ") 
              << (failures > 0 ? std::to_string(failures) : "") << std::endl;
    return failures > 0 ? 1 : 0;
}
