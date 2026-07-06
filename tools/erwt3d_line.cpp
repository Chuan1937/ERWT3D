#include "erwt3d/reader.hpp"
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

namespace {

void printUsage(const char* prog) {
    std::cerr << "Usage: " << prog << " --input file.erwt3d --axis x|y|z --fixed1 N --fixed2 N --output line.raw [options]\n\n"
              << "Read one line along a primary axis and write standard float32 raw output.\n\n"
              << "Axis mapping:\n"
              << "  --axis x : fixed1=y, fixed2=z, output length=nx\n"
              << "  --axis y : fixed1=x, fixed2=z, output length=ny\n"
              << "  --axis z : fixed1=x, fixed2=y, output length=nz\n\n"
              << "Options:\n"
              << "  --threads N            Thread count (default: 1)\n"
              << "  --memory-limit-mb N    Memory limit (default: 2048)\n"
              << "  --io-backend sb|pread  I/O backend (default: sb)\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string inputPath;
    std::string outputPath;
    std::string axisStr;
    uint64_t fixed1 = 0;
    uint64_t fixed2 = 0;
    int numThreads = 1;
    size_t memoryLimitMB = 2048;
    std::string ioBackendStr = "sb";

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--input") == 0 && i + 1 < argc) inputPath = argv[++i];
        else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) outputPath = argv[++i];
        else if (std::strcmp(argv[i], "--axis") == 0 && i + 1 < argc) axisStr = argv[++i];
        else if (std::strcmp(argv[i], "--fixed1") == 0 && i + 1 < argc) fixed1 = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--fixed2") == 0 && i + 1 < argc) fixed2 = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--threads") == 0 && i + 1 < argc) numThreads = std::stoi(argv[++i]);
        else if (std::strcmp(argv[i], "--memory-limit-mb") == 0 && i + 1 < argc) memoryLimitMB = std::stoul(argv[++i]);
        else if (std::strcmp(argv[i], "--io-backend") == 0 && i + 1 < argc) ioBackendStr = argv[++i];
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) { printUsage(argv[0]); return 0; }
        else { std::cerr << "Unknown option: " << argv[i] << std::endl; printUsage(argv[0]); return 1; }
    }

    if (inputPath.empty() || outputPath.empty() || axisStr.empty()) {
        std::cerr << "Error: --input, --output, --axis, --fixed1, --fixed2 are required" << std::endl;
        printUsage(argv[0]);
        return 1;
    }

    erwt3d::ERWT3DReader reader(inputPath);
    if (ioBackendStr == "sb" || ioBackendStr == "superblock") {
        reader.setIOBackend(erwt3d::IOBackend::Superblock);
    } else if (ioBackendStr != "pread") {
        std::cerr << "Error: unknown --io-backend: " << ioBackendStr << " (valid: pread, sb)" << std::endl;
        return 1;
    }

    const auto& h = reader.getHeader();
    erwt3d::SliceAxis axis;
    uint64_t outputLen = 0;
    uint64_t limit1 = 0;
    uint64_t limit2 = 0;
    if (axisStr == "x" || axisStr == "X") {
        axis = erwt3d::SliceAxis::X;
        outputLen = h.nx;
        limit1 = h.ny;
        limit2 = h.nz;
    } else if (axisStr == "y" || axisStr == "Y") {
        axis = erwt3d::SliceAxis::Y;
        outputLen = h.ny;
        limit1 = h.nx;
        limit2 = h.nz;
    } else if (axisStr == "z" || axisStr == "Z") {
        axis = erwt3d::SliceAxis::Z;
        outputLen = h.nz;
        limit1 = h.nx;
        limit2 = h.ny;
    } else {
        std::cerr << "Error: invalid axis: " << axisStr << std::endl;
        return 1;
    }

    if (fixed1 >= limit1 || fixed2 >= limit2) {
        std::cerr << "Error: fixed coordinates out of range for axis " << axisStr
                  << " (fixed1 < " << limit1 << ", fixed2 < " << limit2 << ")" << std::endl;
        return 1;
    }

    std::vector<float> output(outputLen);
    if (!reader.readLine(axis, fixed1, fixed2, output.data(), numThreads, memoryLimitMB)) {
        std::cerr << "Error: line read failed" << std::endl;
        return 1;
    }

    std::ofstream out(outputPath, std::ios::binary);
    out.write(reinterpret_cast<const char*>(output.data()), outputLen * sizeof(float));
    out.close();
    if (!out.good()) {
        std::cerr << "Error: write failed" << std::endl;
        return 1;
    }

    std::cout << "input: " << inputPath << std::endl;
    std::cout << "axis: " << axisStr << std::endl;
    std::cout << "fixed1: " << fixed1 << std::endl;
    std::cout << "fixed2: " << fixed2 << std::endl;
    std::cout << "threads: " << numThreads << std::endl;
    std::cout << "memory_limit_mb: " << memoryLimitMB << std::endl;
    std::cout << "io_backend: " << ioBackendStr << std::endl;
    std::cout << "output_length: " << outputLen << std::endl;
    std::cout << "output_bytes: " << outputLen * sizeof(float) << std::endl;
    std::cout << "output: " << outputPath << std::endl;
    return 0;
}
