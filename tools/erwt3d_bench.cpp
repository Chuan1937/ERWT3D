#include "erwt3d/reader.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <random>
#include <chrono>
#include <algorithm>
#include <numeric>
#include <cstring>
#include <iomanip>
#include <sys/stat.h>

struct BenchmarkResult {
    std::string method;
    std::string axis;
    std::string mode;
    int count;
    double avgTimeMs;
    double minTimeMs;
    double maxTimeMs;
    double totalTimeMs;
    uint64_t outputBytes;
};

void writeCSV(const std::string& path, const std::vector<BenchmarkResult>& results) {
    std::ofstream file(path);
    file << "method,axis,mode,count,avg_time_ms,min_time_ms,max_time_ms,total_time_ms,output_bytes" << std::endl;
    
    for (const auto& r : results) {
        file << r.method << ","
             << r.axis << ","
             << r.mode << ","
             << r.count << ","
             << std::fixed << std::setprecision(3) << r.avgTimeMs << ","
             << r.minTimeMs << ","
             << r.maxTimeMs << ","
             << r.totalTimeMs << ","
             << r.outputBytes << std::endl;
    }
}

void printUsage(const char* progName) {
    std::cerr << "Usage: " << progName << " --input data.erwt3d --output-dir DIR [options]" << std::endl;
    std::cerr << std::endl;
    std::cerr << "Options:" << std::endl;
    std::cerr << "  --random-count N      Number of random slice reads per axis (default: 100)" << std::endl;
    std::cerr << "  --continuous-count N  Number of continuous slice reads per axis (default: 10)" << std::endl;
    std::cerr << "  --threads N           Number of threads (default: 1)" << std::endl;
    std::cerr << "  --memory-limit-mb N   Memory limit in MB (default: 2048)" << std::endl;
    std::cerr << "  --cache-mb N          Cache size in MB (default: 0)" << std::endl;
    std::cerr << "  --seed N              Random seed (default: 20260511)" << std::endl;
}

int main(int argc, char* argv[]) {
    std::string inputPath;
    std::string outputDir;
    int randomCount = 100;
    int continuousCount = 10;
    int numThreads = 1;
    size_t memoryLimitMB = 2048;
    size_t cacheMB = 0;
    uint32_t seed = 20260511;
    
    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            inputPath = argv[++i];
        } else if (std::strcmp(argv[i], "--output-dir") == 0 && i + 1 < argc) {
            outputDir = argv[++i];
        } else if (std::strcmp(argv[i], "--random-count") == 0 && i + 1 < argc) {
            randomCount = std::stoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--continuous-count") == 0 && i + 1 < argc) {
            continuousCount = std::stoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            numThreads = std::stoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--memory-limit-mb") == 0 && i + 1 < argc) {
            memoryLimitMB = std::stoul(argv[++i]);
        } else if (std::strcmp(argv[i], "--cache-mb") == 0 && i + 1 < argc) {
            cacheMB = std::stoul(argv[++i]);
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
    
    if (inputPath.empty() || outputDir.empty()) {
        std::cerr << "Error: --input and --output-dir are required" << std::endl;
        printUsage(argv[0]);
        return 1;
    }
    
    // Create output directory
    std::string mkdirCmd = "mkdir -p " + outputDir;
    system(mkdirCmd.c_str());
    
    // Open reader
    erwt3d::ERWT3DReader reader(inputPath, cacheMB);
    const auto& header = reader.getHeader();
    
    std::cout << "ERWT3D Benchmark" << std::endl;
    std::cout << "===============" << std::endl;
    std::cout << "Dimensions: " << header.nx << " x " << header.ny << " x " << header.nz << std::endl;
    std::cout << "Random slices: " << randomCount << " per axis" << std::endl;
    std::cout << "Continuous slices: " << continuousCount << " per axis" << std::endl;
    std::cout << std::endl;
    
    std::vector<BenchmarkResult> results;
    std::mt19937 rng(seed);
    
    // Benchmark function
    auto benchmarkSlice = [&](erwt3d::SliceAxis axis, const std::string& axisName, 
                              const std::vector<uint64_t>& indices, const std::string& mode) {
        std::vector<double> times;
        uint64_t outputBytes = 0;
        
        for (size_t i = 0; i < indices.size(); ++i) {
            uint64_t idx = indices[i];
            
            // Calculate output size
            uint64_t sliceSize;
            switch (axis) {
                case erwt3d::SliceAxis::X:
                    sliceSize = header.ny * header.nz;
                    break;
                case erwt3d::SliceAxis::Y:
                    sliceSize = header.nx * header.nz;
                    break;
                case erwt3d::SliceAxis::Z:
                    sliceSize = header.nx * header.ny;
                    break;
            }
            
            std::vector<float> output(sliceSize);
            
            // Write output file
            std::string outPath = outputDir + "/" + axisName + "_" + mode + "_" + std::to_string(i) + ".raw";
            
            auto start = std::chrono::high_resolution_clock::now();
            
            if (!reader.readSlice(axis, idx, output.data())) {
                std::cerr << "Error: Failed to read slice" << std::endl;
                return;
            }
            
            // Write to file
            std::ofstream outFile(outPath, std::ios::binary);
            outFile.write(reinterpret_cast<const char*>(output.data()), sliceSize * sizeof(float));
            
            auto end = std::chrono::high_resolution_clock::now();
            double timeMs = std::chrono::duration<double, std::milli>(end - start).count();
            
            times.push_back(timeMs);
            outputBytes = sliceSize * sizeof(float);
        }
        
        // Calculate statistics
        double totalTime = std::accumulate(times.begin(), times.end(), 0.0);
        double avgTime = totalTime / times.size();
        double minTime = *std::min_element(times.begin(), times.end());
        double maxTime = *std::max_element(times.begin(), times.end());
        
        BenchmarkResult result;
        result.method = "erwt3d";
        result.axis = axisName;
        result.mode = mode;
        result.count = indices.size();
        result.avgTimeMs = avgTime;
        result.minTimeMs = minTime;
        result.maxTimeMs = maxTime;
        result.totalTimeMs = totalTime;
        result.outputBytes = outputBytes;
        
        results.push_back(result);
        
        std::cout << axisName << " " << mode << ": avg=" << std::fixed << std::setprecision(2) 
                  << avgTime << "ms, min=" << minTime << "ms, max=" << maxTime << "ms" << std::endl;
    };
    
    // Generate random indices
    std::uniform_int_distribution<uint64_t> distX(0, header.nx - 1);
    std::uniform_int_distribution<uint64_t> distY(0, header.ny - 1);
    std::uniform_int_distribution<uint64_t> distZ(0, header.nz - 1);
    
    std::vector<uint64_t> randomX, randomY, randomZ;
    for (int i = 0; i < randomCount; ++i) {
        randomX.push_back(distX(rng));
        randomY.push_back(distY(rng));
        randomZ.push_back(distZ(rng));
    }
    
    // Generate continuous indices
    std::vector<uint64_t> continuousX, continuousY, continuousZ;
    uint64_t startX = header.nx / 2 - continuousCount / 2;
    uint64_t startY = header.ny / 2 - continuousCount / 2;
    uint64_t startZ = header.nz / 2 - continuousCount / 2;
    
    for (int i = 0; i < continuousCount; ++i) {
        continuousX.push_back(startX + i);
        continuousY.push_back(startY + i);
        continuousZ.push_back(startZ + i);
    }
    
    // Run benchmarks
    std::cout << "\nRunning random slice benchmarks..." << std::endl;
    benchmarkSlice(erwt3d::SliceAxis::X, "x", randomX, "random");
    benchmarkSlice(erwt3d::SliceAxis::Y, "y", randomY, "random");
    benchmarkSlice(erwt3d::SliceAxis::Z, "z", randomZ, "random");
    
    std::cout << "\nRunning continuous slice benchmarks..." << std::endl;
    benchmarkSlice(erwt3d::SliceAxis::X, "x", continuousX, "continuous");
    benchmarkSlice(erwt3d::SliceAxis::Y, "y", continuousY, "continuous");
    benchmarkSlice(erwt3d::SliceAxis::Z, "z", continuousZ, "continuous");
    
    // Write CSV
    std::string csvPath = outputDir + "/bench_result.csv";
    writeCSV(csvPath, results);
    std::cout << "\nResults written to " << csvPath << std::endl;
    
    // Print summary
    std::cout << "\nSummary" << std::endl;
    std::cout << "=======" << std::endl;
    
    double tXRandom = 0, tYRandom = 0, tZRandom = 0;
    double tXCont = 0, tYCont = 0, tZCont = 0;
    
    for (const auto& r : results) {
        if (r.axis == "x" && r.mode == "random") tXRandom = r.avgTimeMs;
        if (r.axis == "y" && r.mode == "random") tYRandom = r.avgTimeMs;
        if (r.axis == "z" && r.mode == "random") tZRandom = r.avgTimeMs;
        if (r.axis == "x" && r.mode == "continuous") tXCont = r.avgTimeMs;
        if (r.axis == "y" && r.mode == "continuous") tYCont = r.avgTimeMs;
        if (r.axis == "z" && r.mode == "continuous") tZCont = r.avgTimeMs;
    }
    
    double tRandomAvg = (tXRandom + tYRandom + tZRandom) / 3.0;
    double tContAvg = (tXCont + tYCont + tZCont) / 3.0;
    double tTotal = (tRandomAvg + tContAvg) / 2.0;
    
    std::cout << "T_x_random: " << std::fixed << std::setprecision(2) << tXRandom << " ms" << std::endl;
    std::cout << "T_y_random: " << tYRandom << " ms" << std::endl;
    std::cout << "T_z_random: " << tZRandom << " ms" << std::endl;
    std::cout << "T_random_avg: " << tRandomAvg << " ms" << std::endl;
    std::cout << std::endl;
    std::cout << "T_x_continuous: " << tXCont << " ms" << std::endl;
    std::cout << "T_y_continuous: " << tYCont << " ms" << std::endl;
    std::cout << "T_z_continuous: " << tZCont << " ms" << std::endl;
    std::cout << "T_cont_avg: " << tContAvg << " ms" << std::endl;
    std::cout << std::endl;
    std::cout << "T_total: " << tTotal << " ms" << std::endl;
    
    // Calculate storage ratio
    uint64_t rawSize = erwt3d::getRawSize(header);
    struct stat st;
    if (stat(inputPath.c_str(), &st) == 0) {
        double ratio = static_cast<double>(st.st_size) / rawSize;
        std::cout << "\nStorage ratio: " << std::fixed << std::setprecision(3) << ratio << "x" << std::endl;
    }
    
    return 0;
}