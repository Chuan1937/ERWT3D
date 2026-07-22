#include "erwt3d/raw_x_aux.hpp"

#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;

#define TEST(name) do { std::cout << "  " << name << "... "; } while(0)
#define PASS() do { std::cout << "PASS" << std::endl; } while(0)
#define FAIL(msg) do { std::cerr << "FAIL: " << msg << std::endl; ++failures; return; } while(0)

void testChunkedReadWriteBasic() {
    TEST("Chunked read/write basic (4 MiB, 1 MiB chunks)");
    const std::string path = "/tmp/test_large_io_basic.bin";
    const uint64_t totalBytes = 4ULL * 1024 * 1024;
    const uint64_t chunkSize = 1ULL * 1024 * 1024;

    std::vector<uint8_t> src(totalBytes);
    for (uint64_t i = 0; i < totalBytes; ++i) src[i] = static_cast<uint8_t>(i & 0xFF);

    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) FAIL("open for write");

    if (!erwt3d::writeFullyAt(fd, src.data(), totalBytes, 0, chunkSize)) {
        close(fd); unlink(path.c_str()); FAIL("writeFullyAt");
    }
    close(fd);

    fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) FAIL("open for read");

    std::vector<uint8_t> dst(totalBytes, 0xAA);
    if (!erwt3d::readFullyAt(fd, dst.data(), totalBytes, 0, chunkSize)) {
        close(fd); unlink(path.c_str()); FAIL("readFullyAt");
    }
    close(fd);

    if (std::memcmp(src.data(), dst.data(), totalBytes) != 0) FAIL("data mismatch");

    unlink(path.c_str());
    PASS();
}

void testChunkedReadWriteLargeOffset() {
    TEST("Chunked read/write at large offset (>2 GiB)");
    const std::string path = "/tmp/test_large_io_offset.bin";
    const uint64_t offset = 3ULL * 1024 * 1024 * 1024;
    const uint64_t dataBytes = 2ULL * 1024 * 1024;
    const uint64_t chunkSize = 512ULL * 1024;

    std::vector<uint8_t> src(dataBytes);
    for (uint64_t i = 0; i < dataBytes; ++i) src[i] = static_cast<uint8_t>((i + 37) & 0xFF);

    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) FAIL("open for write");

    if (!erwt3d::writeFullyAt(fd, src.data(), dataBytes, offset, chunkSize)) {
        close(fd); unlink(path.c_str()); FAIL("writeFullyAt at large offset");
    }
    close(fd);

    fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) FAIL("open for read");

    std::vector<uint8_t> dst(dataBytes, 0);
    if (!erwt3d::readFullyAt(fd, dst.data(), dataBytes, offset, chunkSize)) {
        close(fd); unlink(path.c_str()); FAIL("readFullyAt at large offset");
    }
    close(fd);

    if (std::memcmp(src.data(), dst.data(), dataBytes) != 0) FAIL("data mismatch at large offset");

    unlink(path.c_str());
    PASS();
}

void testDefaultChunkSize() {
    TEST("Default chunk size (no explicit chunk param)");
    const std::string path = "/tmp/test_large_io_default.bin";
    const uint64_t totalBytes = 512ULL * 1024;

    std::vector<uint8_t> src(totalBytes);
    for (uint64_t i = 0; i < totalBytes; ++i) src[i] = static_cast<uint8_t>(i & 0x7F);

    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) FAIL("open");

    if (!erwt3d::writeFullyAt(fd, src.data(), totalBytes, 0)) {
        close(fd); unlink(path.c_str()); FAIL("writeFullyAt default");
    }
    close(fd);

    fd = open(path.c_str(), O_RDONLY);
    std::vector<uint8_t> dst(totalBytes, 0);
    if (!erwt3d::readFullyAt(fd, dst.data(), totalBytes, 0)) {
        close(fd); unlink(path.c_str()); FAIL("readFullyAt default");
    }
    close(fd);

    if (std::memcmp(src.data(), dst.data(), totalBytes) != 0) FAIL("data mismatch");

    unlink(path.c_str());
    PASS();
}

void testSmallChunkSize() {
    TEST("Small chunk size (4 KiB chunks for 1 MiB data)");
    const std::string path = "/tmp/test_large_io_smallchunk.bin";
    const uint64_t totalBytes = 1ULL * 1024 * 1024;
    const uint64_t chunkSize = 4096;

    std::vector<uint8_t> src(totalBytes);
    for (uint64_t i = 0; i < totalBytes; ++i) src[i] = static_cast<uint8_t>((i * 7 + 3) & 0xFF);

    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) FAIL("open");

    if (!erwt3d::writeFullyAt(fd, src.data(), totalBytes, 0, chunkSize)) {
        close(fd); unlink(path.c_str()); FAIL("writeFullyAt small chunk");
    }
    close(fd);

    fd = open(path.c_str(), O_RDONLY);
    std::vector<uint8_t> dst(totalBytes, 0);
    if (!erwt3d::readFullyAt(fd, dst.data(), totalBytes, 0, chunkSize)) {
        close(fd); unlink(path.c_str()); FAIL("readFullyAt small chunk");
    }
    close(fd);

    if (std::memcmp(src.data(), dst.data(), totalBytes) != 0) FAIL("data mismatch");

    unlink(path.c_str());
    PASS();
}

void testReadPastEnd() {
    TEST("Read past end of file returns false");
    const std::string path = "/tmp/test_large_io_pastend.bin";
    const uint64_t fileBytes = 4096;
    const uint64_t readBytes = 8192;

    std::vector<uint8_t> src(fileBytes, 0x42);
    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) FAIL("open");
    erwt3d::writeFullyAt(fd, src.data(), fileBytes, 0);
    close(fd);

    fd = open(path.c_str(), O_RDONLY);
    std::vector<uint8_t> dst(readBytes, 0);
    bool ok = erwt3d::readFullyAt(fd, dst.data(), readBytes, 0, 4096);
    close(fd);
    unlink(path.c_str());

    if (ok) FAIL("should have failed reading past end");
    PASS();
}

void testZeroBytes() {
    TEST("Zero bytes read/write succeeds");
    const std::string path = "/tmp/test_large_io_zero.bin";
    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) FAIL("open");

    bool rOk = erwt3d::readFullyAt(fd, nullptr, 0, 0);
    bool wOk = erwt3d::writeFullyAt(fd, nullptr, 0, 0);
    close(fd);
    unlink(path.c_str());

    if (!rOk || !wOk) FAIL("zero-byte I/O should succeed");
    PASS();
}

} // namespace

int main() {
    std::cout << "test_large_positional_io" << std::endl;

    testChunkedReadWriteBasic();
    testChunkedReadWriteLargeOffset();
    testDefaultChunkSize();
    testSmallChunkSize();
    testReadPastEnd();
    testZeroBytes();

    std::cout << "\n" << (failures == 0 ? "ALL TESTS PASSED" : "FAILURES: ")
              << (failures > 0 ? std::to_string(failures) : "") << std::endl;
    return failures > 0 ? 1 : 0;
}
