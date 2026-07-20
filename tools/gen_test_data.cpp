#include <iostream>
#include <fstream>
#include <vector>
#include <random>
#include <cstdint>
#include <cstring>
#include <string>

void printUsage(const char* progName) {
    std::cerr << "Usage: " << progName << " --nx N --ny N --nz N --output FILE [--seed N]" << std::endl;
    std::cerr << std::endl;
    std::cerr << "Generate test 3D float32 data with random values." << std::endl;
    std::cerr << std::endl;
    std::cerr << "Options:" << std::endl;
    std::cerr << "  --nx N        X dimension (required)" << std::endl;
    std::cerr << "  --ny N        Y dimension (required)" << std::endl;
    std::cerr << "  --nz N        Z dimension (required)" << std::endl;
    std::cerr << "  --output FILE Output file path (required)" << std::endl;
    std::cerr << "  --seed N      Random seed (default: 42)" << std::endl;
}

int main(int argc, char* argv[]) {
    uint64_t nx = 0, ny = 0, nz = 0;
    std::string outputPath;
    uint32_t seed = 42;
    
    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--nx") == 0 && i + 1 < argc) {
            nx = std::stoull(argv[++i]);
        } else if (std::strcmp(argv[i], "--ny") == 0 && i + 1 < argc) {
            ny = std::stoull(argv[++i]);
        } else if (std::strcmp(argv[i], "--nz") == 0 && i + 1 < argc) {
            nz = std::stoull(argv[++i]);
        } else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            outputPath = argv[++i];
        } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = std::stoul(argv[++i]);
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << argv[i] << std::endl;
            printUsage(argv[0]);
            return 1;
        }
    }
    
    if (nx == 0 || ny == 0 || nz == 0 || outputPath.empty()) {
        std::cerr << "Error: --nx, --ny, --nz, and --output are required" << std::endl;
        printUsage(argv[0]);
        return 1;
    }
    
    uint64_t totalElements = nx * ny * nz;
    uint64_t fileSize = totalElements * sizeof(float);
    
    std::cout << "Generating 3D test data..." << std::endl;
    std::cout << "Dimensions: " << nx << " x " << ny << " x " << nz << std::endl;
    std::cout << "Total elements: " << totalElements << std::endl;
    std::cout << "File size: " << fileSize / (1024*1024) << " MB" << std::endl;
    std::cout << "Seed: " << seed << std::endl;
    
    // Initialize random generator
    std::mt19937 gen(seed);
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    
    // Open output file
    std::ofstream file(outputPath, std::ios::binary);
    if (!file) {
        std::cerr << "Error: Cannot open output file: " << outputPath << std::endl;
        return 1;
    }
    
    // Write data in X-Y-Z order
    constexpr size_t BUFFER_SIZE = 65536;
    std::vector<float> buffer;
    buffer.reserve(BUFFER_SIZE);
    
    uint64_t processed = 0;
    int lastPercent = 0;
    
    for (uint64_t x = 0; x < nx; ++x) {
        for (uint64_t y = 0; y < ny; ++y) {
            for (uint64_t z = 0; z < nz; ++z) {
                buffer.push_back(dist(gen));
                processed++;
                
                if (buffer.size() >= BUFFER_SIZE) {
                    file.write(reinterpret_cast<const char*>(buffer.data()), 
                              buffer.size() * sizeof(float));
                    buffer.clear();
                }
                
                // Update progress
                int percent = static_cast<int>(processed * 100 / totalElements);
                if (percent != lastPercent) {
                    lastPercent = percent;
                    std::cout << "\rProgress: " << percent << "%" << std::flush;
                }
            }
        }
    }
    
    // Write remaining data
    if (!buffer.empty()) {
        file.write(reinterpret_cast<const char*>(buffer.data()), 
                  buffer.size() * sizeof(float));
    }
    
    file.close();
    std::cout << "\rProgress: 100%" << std::endl;
    std::cout << "Data generation complete: " << outputPath << std::endl;
    
    return 0;
}