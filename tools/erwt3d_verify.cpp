#include "erwt3d/reader.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstring>

void printUsage(const char* progName) {
    std::cerr << "Usage:" << std::endl;
    std::cerr << "  Compare raw file with ERWT3D:" << std::endl;
    std::cerr << "    " << progName << " --raw data.raw --erwt3d data.erwt3d --nx N --ny N --nz N [--samples N]" << std::endl;
    std::cerr << std::endl;
    std::cerr << "  Compare two raw files:" << std::endl;
    std::cerr << "    " << progName << " --raw-a data.raw --raw-b restored.raw --nx N --ny N --nz N" << std::endl;
}

int main(int argc, char* argv[]) {
    std::string rawPath;
    std::string erwt3dPath;
    std::string rawAPath, rawBPath;
    uint64_t nx = 0, ny = 0, nz = 0;
    uint64_t numSamples = 0;
    
    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--raw") == 0 && i + 1 < argc) {
            rawPath = argv[++i];
        } else if (std::strcmp(argv[i], "--erwt3d") == 0 && i + 1 < argc) {
            erwt3dPath = argv[++i];
        } else if (std::strcmp(argv[i], "--raw-a") == 0 && i + 1 < argc) {
            rawAPath = argv[++i];
        } else if (std::strcmp(argv[i], "--raw-b") == 0 && i + 1 < argc) {
            rawBPath = argv[++i];
        } else if (std::strcmp(argv[i], "--nx") == 0 && i + 1 < argc) {
            nx = std::stoull(argv[++i]);
        } else if (std::strcmp(argv[i], "--ny") == 0 && i + 1 < argc) {
            ny = std::stoull(argv[++i]);
        } else if (std::strcmp(argv[i], "--nz") == 0 && i + 1 < argc) {
            nz = std::stoull(argv[++i]);
        } else if (std::strcmp(argv[i], "--samples") == 0 && i + 1 < argc) {
            numSamples = std::stoull(argv[++i]);
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << argv[i] << std::endl;
            printUsage(argv[0]);
            return 1;
        }
    }
    
    if (nx == 0 || ny == 0 || nz == 0) {
        std::cerr << "Error: --nx, --ny, --nz are required" << std::endl;
        printUsage(argv[0]);
        return 1;
    }
    
    uint64_t totalElements = nx * ny * nz;
    
    if (!rawPath.empty() && !erwt3dPath.empty()) {
        // Compare raw file with ERWT3D
        std::cout << "Comparing raw file with ERWT3D..." << std::endl;
        
        // Read raw file
        std::ifstream rawFile(rawPath, std::ios::binary);
        if (!rawFile) {
            std::cerr << "Error: Cannot open raw file" << std::endl;
            return 1;
        }
        
        std::vector<float> rawData(totalElements);
        rawFile.read(reinterpret_cast<char*>(rawData.data()), totalElements * sizeof(float));
        if (!rawFile) {
            std::cerr << "Error: Failed to read raw file" << std::endl;
            return 1;
        }
        
        // Read ERWT3D
        erwt3d::ERWT3DReader reader(erwt3dPath);
        std::vector<float> erwt3dData(totalElements);
        if (!reader.readFull(erwt3dData.data())) {
            std::cerr << "Error: Failed to read ERWT3D file" << std::endl;
            return 1;
        }
        
        // Compare
        double maxAbsError = 0.0;
        double maxRelError = 0.0;
        uint64_t numFailed = 0;
        
        if (numSamples > 0 && numSamples < totalElements) {
            // Random sampling
            srand(42);
            for (uint64_t i = 0; i < numSamples; ++i) {
                uint64_t idx = rand() % totalElements;
                double diff = std::abs(rawData[idx] - erwt3dData[idx]);
                double relDiff = diff / (std::abs(rawData[idx]) + 1e-10);
                
                maxAbsError = std::max(maxAbsError, diff);
                maxRelError = std::max(maxRelError, relDiff);
                
                if (diff > 1e-3) {
                    ++numFailed;
                }
            }
        } else {
            // Full comparison
            for (uint64_t i = 0; i < totalElements; ++i) {
                double diff = std::abs(rawData[i] - erwt3dData[i]);
                double relDiff = diff / (std::abs(rawData[i]) + 1e-10);
                
                maxAbsError = std::max(maxAbsError, diff);
                maxRelError = std::max(maxRelError, relDiff);
                
                if (diff > 1e-3) {
                    ++numFailed;
                }
            }
        }
        
        // Report
        std::cout << "max_abs_error: " << maxAbsError << std::endl;
        std::cout << "max_rel_error: " << maxRelError << std::endl;
        std::cout << "num_failed: " << numFailed << std::endl;
        std::cout << "passed: " << (numFailed == 0 ? "true" : "false") << std::endl;
        
        return numFailed == 0 ? 0 : 1;
        
    } else if (!rawAPath.empty() && !rawBPath.empty()) {
        // Compare two raw files
        std::cout << "Comparing two raw files..." << std::endl;
        
        std::ifstream rawAFile(rawAPath, std::ios::binary);
        std::ifstream rawBFile(rawBPath, std::ios::binary);
        
        if (!rawAFile || !rawBFile) {
            std::cerr << "Error: Cannot open raw files" << std::endl;
            return 1;
        }
        
        std::vector<float> rawA(totalElements);
        std::vector<float> rawB(totalElements);
        
        rawAFile.read(reinterpret_cast<char*>(rawA.data()), totalElements * sizeof(float));
        rawBFile.read(reinterpret_cast<char*>(rawB.data()), totalElements * sizeof(float));
        
        if (!rawAFile || !rawBFile) {
            std::cerr << "Error: Failed to read raw files" << std::endl;
            return 1;
        }
        
        // Compare
        double maxAbsError = 0.0;
        double maxRelError = 0.0;
        uint64_t numFailed = 0;
        
        for (uint64_t i = 0; i < totalElements; ++i) {
            double diff = std::abs(rawA[i] - rawB[i]);
            double relDiff = diff / (std::abs(rawA[i]) + 1e-10);
            
            maxAbsError = std::max(maxAbsError, diff);
            maxRelError = std::max(maxRelError, relDiff);
            
            if (diff > 1e-3) {
                ++numFailed;
            }
        }
        
        // Report
        std::cout << "max_abs_error: " << maxAbsError << std::endl;
        std::cout << "max_rel_error: " << maxRelError << std::endl;
        std::cout << "num_failed: " << numFailed << std::endl;
        std::cout << "passed: " << (numFailed == 0 ? "true" : "false") << std::endl;
        
        return numFailed == 0 ? 0 : 1;
        
    } else {
        std::cerr << "Error: Must specify either --raw/--erwt3d or --raw-a/--raw-b" << std::endl;
        printUsage(argv[0]);
        return 1;
    }
}