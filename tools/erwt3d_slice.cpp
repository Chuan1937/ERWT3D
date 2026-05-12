#include "erwt3d/reader.hpp"
#include <iostream>
#include <fstream>
#include <string>
#include <cstring>

void printUsage(const char* progName) {
    std::cerr << "Usage:" << std::endl;
    std::cerr << "  Read a slice:" << std::endl;
    std::cerr << "    " << progName << " --input data.erwt3d --axis X|Y|Z --index N --output slice.raw" << std::endl;
    std::cerr << std::endl;
    std::cerr << "  Read a line along X:" << std::endl;
    std::cerr << "    " << progName << " --input data.erwt3d --line-x --y N --z N --output line.raw" << std::endl;
}

int main(int argc, char* argv[]) {
    std::string inputPath;
    std::string outputPath;
    std::string axisStr;
    uint64_t index = 0;
    bool lineX = false;
    uint64_t lineY = 0, lineZ = 0;
    int numThreads = 1;
    size_t memoryLimitMB = 2048;
    
    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            inputPath = argv[++i];
        } else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            outputPath = argv[++i];
        } else if (std::strcmp(argv[i], "--axis") == 0 && i + 1 < argc) {
            axisStr = argv[++i];
        } else if (std::strcmp(argv[i], "--index") == 0 && i + 1 < argc) {
            index = std::stoull(argv[++i]);
        } else if (std::strcmp(argv[i], "--line-x") == 0) {
            lineX = true;
        } else if (std::strcmp(argv[i], "--y") == 0 && i + 1 < argc) {
            lineY = std::stoull(argv[++i]);
        } else if (std::strcmp(argv[i], "--z") == 0 && i + 1 < argc) {
            lineZ = std::stoull(argv[++i]);
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            return 0;
        } else if (std::strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            numThreads = std::stoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--memory-limit-mb") == 0 && i + 1 < argc) {
            memoryLimitMB = std::stoul(argv[++i]);
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
    
    erwt3d::ERWT3DReader reader(inputPath);
    const auto& header = reader.getHeader();
    
    if (lineX) {
        // Read X line
        std::cout << "Reading X line at y=" << lineY << ", z=" << lineZ << std::endl;
        
        std::vector<float> output(header.nx);
        if (!reader.readLineX(lineY, lineZ, output.data(), numThreads, memoryLimitMB)) {
            std::cerr << "Error: Failed to read line" << std::endl;
            return 1;
        }
        
        std::ofstream outFile(outputPath, std::ios::binary);
        if (!outFile) {
            std::cerr << "Error: Cannot open output file" << std::endl;
            return 1;
        }
        
        outFile.write(reinterpret_cast<const char*>(output.data()), header.nx * sizeof(float));
        std::cout << "Line written to " << outputPath << std::endl;
    } else {
        // Read slice
        if (axisStr.empty()) {
            std::cerr << "Error: --axis is required for slice read" << std::endl;
            printUsage(argv[0]);
            return 1;
        }
        
        erwt3d::SliceAxis axis;
        if (axisStr == "X" || axisStr == "x") {
            axis = erwt3d::SliceAxis::X;
        } else if (axisStr == "Y" || axisStr == "y") {
            axis = erwt3d::SliceAxis::Y;
        } else if (axisStr == "Z" || axisStr == "z") {
            axis = erwt3d::SliceAxis::Z;
        } else {
            std::cerr << "Error: Invalid axis. Must be X, Y, or Z" << std::endl;
            return 1;
        }
        
        // Calculate output size
        uint64_t outputSize;
        switch (axis) {
            case erwt3d::SliceAxis::X:
                outputSize = header.ny * header.nz;
                std::cout << "Reading X slice at index " << index << std::endl;
                std::cout << "Output size: " << header.ny << " x " << header.nz << std::endl;
                break;
            case erwt3d::SliceAxis::Y:
                outputSize = header.nx * header.nz;
                std::cout << "Reading Y slice at index " << index << std::endl;
                std::cout << "Output size: " << header.nx << " x " << header.nz << std::endl;
                break;
            case erwt3d::SliceAxis::Z:
                outputSize = header.nx * header.ny;
                std::cout << "Reading Z slice at index " << index << std::endl;
                std::cout << "Output size: " << header.nx << " x " << header.ny << std::endl;
                break;
        }
        
        std::vector<float> output(outputSize);
        if (!reader.readSlice(axis, index, output.data(), numThreads, memoryLimitMB)) {
            std::cerr << "Error: Failed to read slice" << std::endl;
            return 1;
        }
        
        std::ofstream outFile(outputPath, std::ios::binary);
        if (!outFile) {
            std::cerr << "Error: Cannot open output file" << std::endl;
            return 1;
        }
        
        outFile.write(reinterpret_cast<const char*>(output.data()), outputSize * sizeof(float));
        std::cout << "Slice written to " << outputPath << std::endl;
    }
    
    return 0;
}