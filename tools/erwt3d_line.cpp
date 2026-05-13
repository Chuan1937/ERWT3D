#include "erwt3d/reader.hpp"
#include <iostream>
#include <fstream>
#include <cstring>

int main(int argc, char* argv[]) {
    std::string inputPath, outputPath, axisStr;
    uint64_t fixed1 = 0, fixed2 = 0;
    int numThreads = 1;
    size_t memoryLimitMB = 2048;
    std::string ioBackendStr = "sb";
    std::string sbParallelModeStr = "serial";

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--input") == 0 && i+1 < argc) inputPath = argv[++i];
        else if (std::strcmp(argv[i], "--output") == 0 && i+1 < argc) outputPath = argv[++i];
        else if (std::strcmp(argv[i], "--axis") == 0 && i+1 < argc) axisStr = argv[++i];
        else if (std::strcmp(argv[i], "--fixed1") == 0 && i+1 < argc) fixed1 = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--fixed2") == 0 && i+1 < argc) fixed2 = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--threads") == 0 && i+1 < argc) numThreads = std::stoi(argv[++i]);
        else if (std::strcmp(argv[i], "--memory-limit-mb") == 0 && i+1 < argc) memoryLimitMB = std::stoul(argv[++i]);
        else if (std::strcmp(argv[i], "--io-backend") == 0 && i+1 < argc) ioBackendStr = argv[++i];
        else if (std::strcmp(argv[i], "--sb-parallel-mode") == 0 && i+1 < argc) sbParallelModeStr = argv[++i];
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::cerr << "Usage: " << argv[0] << " --input file.erwt3d --axis x|y|z --fixed1 N --fixed2 N --output line.raw" << std::endl;
            std::cerr << "  --axis x: fixed1=y, fixed2=z; output length = nx" << std::endl;
            std::cerr << "  --axis y: fixed1=x, fixed2=z; output length = ny" << std::endl;
            std::cerr << "  --axis z: fixed1=x, fixed2=y; output length = nz" << std::endl;
            std::cerr << "  --threads N  --memory-limit-mb N  --io-backend sb|pread  --sb-parallel-mode serial|parallel-read" << std::endl;
            return 0;
        } else {
            std::cerr << "Unknown: " << argv[i] << std::endl; return 1;
        }
    }

    if (inputPath.empty() || outputPath.empty() || axisStr.empty()) {
        std::cerr << "Error: --input, --output, --axis, --fixed1, --fixed2 required" << std::endl;
        return 1;
    }

    erwt3d::ERWT3DReader reader(inputPath);
    if (ioBackendStr == "sb" || ioBackendStr == "superblock") {
        reader.setIOBackend(erwt3d::IOBackend::Superblock);
    } else if (ioBackendStr != "pread") {
        std::cerr << "Error: unknown --io-backend: " << ioBackendStr << " (valid: pread, sb)" << std::endl;
        return 1;
    }
    if (sbParallelModeStr == "parallel-read") {
        reader.setSBParallelMode(erwt3d::SBParallelMode::ParallelRead);
    } else if (sbParallelModeStr != "serial") {
        std::cerr << "Error: unknown --sb-parallel-mode: " << sbParallelModeStr << " (valid: serial, parallel-read)" << std::endl;
        return 1;
    }

    const auto& h = reader.getHeader();
    erwt3d::SliceAxis axis;
    uint64_t outputLen;
    if (axisStr == "x" || axisStr == "X") { axis = erwt3d::SliceAxis::X; outputLen = h.nx; }
    else if (axisStr == "y" || axisStr == "Y") { axis = erwt3d::SliceAxis::Y; outputLen = h.ny; }
    else if (axisStr == "z" || axisStr == "Z") { axis = erwt3d::SliceAxis::Z; outputLen = h.nz; }
    else { std::cerr << "Error: invalid axis: " << axisStr << std::endl; return 1; }

    std::vector<float> output(outputLen);
    if (!reader.readLine(axis, fixed1, fixed2, output.data(), numThreads, memoryLimitMB)) {
        std::cerr << "Error: line read failed" << std::endl;
        return 1;
    }

    std::ofstream out(outputPath, std::ios::binary);
    out.write(reinterpret_cast<const char*>(output.data()), outputLen * sizeof(float));
    out.close();
    if (!out.good()) { std::cerr << "Error: write failed" << std::endl; return 1; }

    std::cout << "Line written to " << outputPath << " (" << outputLen << " floats)" << std::endl;
    return 0;
}
