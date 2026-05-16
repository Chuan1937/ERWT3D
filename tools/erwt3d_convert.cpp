#include "erwt3d/writer.hpp"
#include "erwt3d/reader.hpp"
#include <iostream>
#include <string>
#include <cstring>

void printUsage(const char* progName) {
    std::cerr << "Usage:" << std::endl;
    std::cerr << "  Convert raw to ERWT3D:" << std::endl;
    std::cerr << "    " << progName << " --input data.raw --output data.erwt3d --nx N --ny N --nz N [options]" << std::endl;
    std::cerr << std::endl;
    std::cerr << "  Convert ERWT3D to raw:" << std::endl;
    std::cerr << "    " << progName << " --input data.erwt3d --output restored.raw --to-raw [options]" << std::endl;
    std::cerr << std::endl;
    std::cerr << "Options:" << std::endl;
    std::cerr << "  --nx N              X dimension (required for raw->erwt3d)" << std::endl;
    std::cerr << "  --ny N              Y dimension (required for raw->erwt3d)" << std::endl;
    std::cerr << "  --nz N              Z dimension (required for raw->erwt3d)" << std::endl;
    std::cerr << "  --dtype TYPE        Data type (default: float32)" << std::endl;
    std::cerr << "  --layout LAYOUT     Input layout (default: xyz)" << std::endl;
    std::cerr << "  --threads N         Number of threads (default: 1)" << std::endl;
    std::cerr << "  --memory-limit-mb N Memory limit in MB (default: 2048)" << std::endl;
    std::cerr << "  --super-size N      Superblock size (default: 64)" << std::endl;
    std::cerr << "  --leaf-size N       Leaf block size (default: 4)" << std::endl;
    std::cerr << "  --panel-axis x       Enable X micro-panels (only x supported)" << std::endl;
    std::cerr << "  --panel-stride N     Store every Nth local X plane (must divide super-size)" << std::endl;
}

int main(int argc, char* argv[]) {
    std::string inputPath;
    std::string outputPath;
    uint64_t nx = 0, ny = 0, nz = 0;
    bool toRaw = false;
    int numThreads = 1;
    size_t memoryLimitMB = 2048;
    uint32_t superSize = 64;
    uint32_t tileX = 0, tileY = 0, tileZ = 0;
    uint32_t leafSize = 4;
    uint32_t panelAxis = 0;
    uint32_t panelStride = 0;
    
    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            inputPath = argv[++i];
        } else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            outputPath = argv[++i];
        } else if (std::strcmp(argv[i], "--nx") == 0 && i + 1 < argc) {
            nx = std::stoull(argv[++i]);
        } else if (std::strcmp(argv[i], "--ny") == 0 && i + 1 < argc) {
            ny = std::stoull(argv[++i]);
        } else if (std::strcmp(argv[i], "--nz") == 0 && i + 1 < argc) {
            nz = std::stoull(argv[++i]);
        } else if (std::strcmp(argv[i], "--to-raw") == 0) {
            toRaw = true;
        } else if (std::strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            numThreads = std::stoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--memory-limit-mb") == 0 && i + 1 < argc) {
            memoryLimitMB = std::stoul(argv[++i]);
        } else if (std::strcmp(argv[i], "--super-size") == 0 && i + 1 < argc) {
            superSize = std::stoul(argv[++i]);
        } else if (std::strcmp(argv[i], "--tile-x") == 0 && i + 1 < argc) {
            tileX = std::stoul(argv[++i]);
        } else if (std::strcmp(argv[i], "--tile-y") == 0 && i + 1 < argc) {
            tileY = std::stoul(argv[++i]);
        } else if (std::strcmp(argv[i], "--tile-z") == 0 && i + 1 < argc) {
            tileZ = std::stoul(argv[++i]);
        } else if (std::strcmp(argv[i], "--leaf-size") == 0 && i + 1 < argc) {
            leafSize = std::stoul(argv[++i]);
        } else if (std::strcmp(argv[i], "--panel-axis") == 0 && i + 1 < argc) {
            std::string ax = argv[++i];
            if (ax == "x" || ax == "X") panelAxis = 0;
            else if (ax == "y" || ax == "Y" || ax == "z" || ax == "Z") {
                std::cerr << "Error: only --panel-axis x is currently implemented" << std::endl;
                return 1;
            } else {
                std::cerr << "Error: unknown --panel-axis: " << ax << " (valid: x)" << std::endl;
                return 1;
            }
        } else if (std::strcmp(argv[i], "--panel-stride") == 0 && i + 1 < argc) {
            panelStride = std::stoul(argv[++i]);
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << argv[i] << std::endl;
            printUsage(argv[0]);
            return 1;
        }
    }
    
    if (inputPath.empty() || outputPath.empty()) {
        std::cerr << "Error: --input and --output are required" << std::endl;
        printUsage(argv[0]);
        return 1;
    }
    
    if (toRaw) {
        // Convert ERWT3D to raw
        std::cout << "Converting ERWT3D to raw..." << std::endl;
        erwt3d::ERWT3DReader reader(inputPath);
        
        if (!reader.readFullToFile(outputPath, numThreads, memoryLimitMB)) {
            std::cerr << "Error: Failed to convert ERWT3D to raw" << std::endl;
            return 1;
        }
        
        std::cout << "Conversion complete: " << outputPath << std::endl;
    } else {
        // Convert raw to ERWT3D
        if (nx == 0 || ny == 0 || nz == 0) {
            std::cerr << "Error: --nx, --ny, --nz are required for raw to ERWT3D conversion" << std::endl;
            printUsage(argv[0]);
            return 1;
        }
        
        std::cout << "Converting raw to ERWT3D..." << std::endl;
        std::cout << "Dimensions: " << nx << " x " << ny << " x " << nz << std::endl;
        
        if (!erwt3d::writeERWT3DFromFile(outputPath, inputPath, nx, ny, nz,
                                         tileX ? tileX : superSize,
                                         tileY ? tileY : superSize,
                                         tileZ ? tileZ : superSize,
                                         leafSize, leafSize, leafSize,
                                         numThreads, memoryLimitMB,
                                         panelAxis, panelStride)) {
            std::cerr << "Error: Failed to convert raw to ERWT3D" << std::endl;
            return 1;
        }
        
        std::cout << "Conversion complete: " << outputPath << std::endl;
    }
    
    return 0;
}