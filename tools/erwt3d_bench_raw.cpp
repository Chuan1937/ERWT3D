#include <iostream>
#include <fstream>
#include <vector>
#include <random>
#include <chrono>
#include <algorithm>
#include <numeric>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

struct RawBenchResult {
    std::string axis;
    std::string mode;
    int count;
    double avgTimeMs;
    double minTimeMs;
    double maxTimeMs;
    double totalTimeMs;
    uint64_t outputBytes;
};

bool writeCSV(const std::string& path, const std::vector<RawBenchResult>& results) {
    std::ofstream file(path);
    if (!file) { std::cerr << "Error: Cannot write " << path << std::endl; return false; }
    file << "method,axis,mode,count,avg_time_ms,min_time_ms,max_time_ms,total_time_ms,output_bytes" << std::endl;
    for (const auto& r : results) {
        file << "raw_row_major," << r.axis << "," << r.mode << "," << r.count << ","
             << std::fixed << std::setprecision(3) << r.avgTimeMs << ","
             << r.minTimeMs << "," << r.maxTimeMs << "," << r.totalTimeMs << ","
             << r.outputBytes << std::endl;
    }
    file.close();
    if (!file.good()) { std::cerr << "Error: Failed to write " << path << std::endl; return false; }
    return true;
}

void printUsage(const char* prog) {
    std::cerr << "Usage: " << prog << " --input data.raw --nx N --ny N --nz N --output-dir DIR [options]" << std::endl;
    std::cerr << "Options:" << std::endl;
    std::cerr << "  --random-count N     (default: 100)" << std::endl;
    std::cerr << "  --continuous-count N (default: 10)" << std::endl;
    std::cerr << "  --seed N             (default: 42)" << std::endl;
}

int main(int argc, char* argv[]) {
    std::string inputPath, outputDir;
    uint64_t nx = 0, ny = 0, nz = 0;
    int randomCount = 100, continuousCount = 10;
    uint32_t seed = 42;
    
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--input") == 0 && i+1<argc) inputPath = argv[++i];
        else if (std::strcmp(argv[i], "--output-dir") == 0 && i+1<argc) outputDir = argv[++i];
        else if (std::strcmp(argv[i], "--nx") == 0 && i+1<argc) nx = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--ny") == 0 && i+1<argc) ny = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--nz") == 0 && i+1<argc) nz = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--random-count") == 0 && i+1<argc) randomCount = std::stoi(argv[++i]);
        else if (std::strcmp(argv[i], "--continuous-count") == 0 && i+1<argc) continuousCount = std::stoi(argv[++i]);
        else if (std::strcmp(argv[i], "--seed") == 0 && i+1<argc) seed = std::stoul(argv[++i]);
        else { printUsage(argv[0]); return 1; }
    }
    
    if (inputPath.empty() || outputDir.empty() || nx == 0 || ny == 0 || nz == 0) {
        printUsage(argv[0]); return 1;
    }
    
    std::error_code ec;
    std::filesystem::create_directories(outputDir, ec);
    if (ec) { std::cerr << "Error: " << ec.message() << std::endl; return 1; }
    
    std::cout << "Raw Row-Major Baseline Benchmark" << std::endl;
    std::cout << "=================================" << std::endl;
    std::cout << "Input: " << inputPath << std::endl;
    std::cout << "Dimensions: " << nx << " x " << ny << " x " << nz << std::endl;
    
    // Memory-map or pread raw file for slice extraction
    int fd = open(inputPath.c_str(), O_RDONLY);
    if (fd < 0) { std::cerr << "Error: Cannot open input file" << std::endl; return 1; }
    
    uint64_t fileSize = nx * ny * nz * sizeof(float);
    struct stat st;
    if (fstat(fd, &st) != 0 || static_cast<uint64_t>(st.st_size) < fileSize) {
        std::cerr << "Error: Input file too small" << std::endl;
        close(fd); return 1;
    }
    
    std::mt19937 rng(seed);
    std::vector<RawBenchResult> results;
    std::vector<std::string> detailLines;
    
    auto run = [&](const char* axisName, uint64_t dimSize, uint64_t outD1, uint64_t outD2,
                   bool isY, bool isZ, // Y: read x*z, Z: read x*y
                   const std::vector<uint64_t>& indices, const std::string& mode) -> bool {
        std::vector<double> times;
        uint64_t outBytes = 0;
        
        for (size_t it = 0; it < indices.size(); ++it) {
            uint64_t idx = indices[it];
            uint64_t sliceSize = outD1 * outD2;
            outBytes = sliceSize * sizeof(float);
            std::vector<float> output(sliceSize);
            
            std::string outPath = outputDir + "/raw_" + axisName + "_" + mode + "_" + std::to_string(it) + ".raw";
            
            auto start = std::chrono::high_resolution_clock::now();
            
            // Read slice from raw row-major file
            if (isZ) {
                // Z slice: all x,y at fixed z. Raw layout: (z*ny+y)*nx+x
                uint64_t baseOff = idx * ny * nx;
                for (uint64_t y = 0; y < outD1; ++y) {
                    uint64_t off = (baseOff + y * nx) * sizeof(float);
                    ssize_t n = pread(fd, output.data() + y * outD2, outD2 * sizeof(float), off);
                    if (n != static_cast<ssize_t>(outD2 * sizeof(float))) { close(fd); return false; }
                }
            } else if (isY) {
                // Y slice: all x,z at fixed y. Each z has one contiguous row of nx floats.
                for (uint64_t z = 0; z < outD1; ++z) {
                    uint64_t off = ((z * ny + idx) * nx) * sizeof(float);
                    ssize_t n = pread(fd, output.data() + z * outD2, outD2 * sizeof(float), off);
                    if (n != static_cast<ssize_t>(outD2 * sizeof(float))) { close(fd); return false; }
                }
            } else {
                // X slice: all y,z at fixed x. One float at a time (non-contiguous in raw).
                for (uint64_t z = 0; z < outD1; ++z) {
                    for (uint64_t y = 0; y < outD2; ++y) {
                        uint64_t off = ((z * ny + y) * nx + idx) * sizeof(float);
                        ssize_t n = pread(fd, &output[z * outD2 + y], sizeof(float), off);
                        if (n != sizeof(float)) { close(fd); return false; }
                    }
                }
            }
            
            // Write output
            std::ofstream outFile(outPath, std::ios::binary);
            if (!outFile) { close(fd); return false; }
            outFile.write(reinterpret_cast<const char*>(output.data()), outBytes);
            outFile.close();
            if (!outFile.good()) { close(fd); return false; }
            
            auto end = std::chrono::high_resolution_clock::now();
            double t = std::chrono::duration<double, std::milli>(end - start).count();
            times.push_back(t);
            
            std::ostringstream dl;
            dl << axisName << "," << mode << "," << it << "," << idx << ","
               << std::fixed << std::setprecision(3) << t << "," << outBytes;
            detailLines.push_back(dl.str());
        }
        
        double total = 0; for (auto t : times) total += t;
        double avg = total / times.size();
        double mn = *std::min_element(times.begin(), times.end());
        double mx = *std::max_element(times.begin(), times.end());
        
        RawBenchResult r;
        r.axis = axisName; r.mode = mode; r.count = (int)times.size();
        r.avgTimeMs = avg; r.minTimeMs = mn; r.maxTimeMs = mx; r.totalTimeMs = total;
        r.outputBytes = outBytes;
        results.push_back(r);
        
        std::cout << axisName << " " << mode << ": avg=" << std::fixed << std::setprecision(2) << avg << "ms" << std::endl;
        return true;
    };
    
    // Generate random indices
    std::uniform_int_distribution<uint64_t> dx(0, nx-1), dy_(0, ny-1), dz_(0, nz-1);
    std::vector<uint64_t> rx(randomCount), ry(randomCount), rz(randomCount);
    for (int i = 0; i < randomCount; ++i) { rx[i]=dx(rng); ry[i]=dy_(rng); rz[i]=dz_(rng); }
    
    // Continuous indices (safe against underflow and overflow)
    auto safeStart = [](uint64_t dim, int cnt) -> uint64_t {
        if (static_cast<uint64_t>(cnt) >= dim) return 0;
        return dim / 2 - cnt / 2;
    };
    std::vector<uint64_t> cx, cy, cz;
    uint64_t sx = safeStart(nx, continuousCount), sy = safeStart(ny, continuousCount), sz2_v = safeStart(nz, continuousCount);
    int countX = std::min(continuousCount, static_cast<int>(nx));
    int countY = std::min(continuousCount, static_cast<int>(ny));
    int countZ = std::min(continuousCount, static_cast<int>(nz));
    for (int i = 0; i < countX; ++i) cx.push_back(sx + i);
    for (int i = 0; i < countY; ++i) cy.push_back(sy + i);
    for (int i = 0; i < countZ; ++i) cz.push_back(sz2_v + i);
    
    std::cout << "\nRandom slices:" << std::endl;
    if (!run("x", nx, nz, ny, false, false, rx, "random")) { close(fd); return 1; }
    if (!run("y", ny, nz, nx, true, false, ry, "random")) { close(fd); return 1; }
    if (!run("z", nz, ny, nx, false, true, rz, "random")) { close(fd); return 1; }
    
    std::cout << "\nContinuous slices:" << std::endl;
    if (!run("x", nx, nz, ny, false, false, cx, "continuous")) { close(fd); return 1; }
    if (!run("y", ny, nz, nx, true, false, cy, "continuous")) { close(fd); return 1; }
    if (!run("z", nz, ny, nx, false, true, cz, "continuous")) { close(fd); return 1; }
    
    close(fd);
    
    // Write CSVs
    std::string csvPath = outputDir + "/bench_result.csv";
    if (!writeCSV(csvPath, results)) return 1;
    std::string detPath = outputDir + "/bench_detail.csv";
    std::ofstream df(detPath);
    if (!df) { std::cerr << "Error: Cannot write detail CSV" << std::endl; return 1; }
    df << "axis,mode,iteration,index,time_ms,output_bytes" << std::endl;
    for (const auto& l : detailLines) df << l << std::endl;
    if (!df.good()) { std::cerr << "Error: Detail CSV write failed" << std::endl; return 1; }
    
    // Summary
    std::cout << "\nResults: " << csvPath << std::endl;
    std::cout << "Details: " << detPath << std::endl;
    return 0;
}