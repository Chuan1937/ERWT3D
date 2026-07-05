#include "erwt3d/reader.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

void printUsage(const char* prog) {
    std::cerr << "Usage: " << prog << " --input PATH --axis x|y|z --output-dir DIR [options]\n\n"
              << "Benchmark primary-axis line reads. This is not part of the 60-point contest formula.\n\n"
              << "Options:\n"
              << "  --count N                 Number of random line reads (default: 100)\n"
              << "  --seed N                  RNG seed (default: 20260511)\n"
              << "  --threads N               Thread count (default: 1)\n"
              << "  --memory-limit-mb N       Memory limit (default: 2048)\n"
              << "  --io-backend pread|sb     I/O backend (default: sb)\n";
}

bool writeRaw(const std::string& path, const float* data, uint64_t bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data), bytes);
    out.close();
    return out.good();
}

double medianOf(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    size_t mid = values.size() / 2;
    if (values.size() % 2 == 0) {
        return (values[mid - 1] + values[mid]) / 2.0;
    }
    return values[mid];
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string inputPath;
    std::string axisStr;
    std::string outputDir;
    int count = 100;
    uint32_t seed = 20260511;
    int numThreads = 1;
    size_t memoryLimitMB = 2048;
    std::string ioBackendStr = "sb";

    for (int i = 1; i < argc; ++i) {
        auto next = [&]() -> const char* {
            if (i + 1 < argc) {
                return argv[++i];
            }
            std::cerr << "Error: " << argv[i] << " requires a value\n";
            std::exit(1);
            return nullptr;
        };

        if (std::strcmp(argv[i], "--input") == 0) inputPath = next();
        else if (std::strcmp(argv[i], "--axis") == 0) axisStr = next();
        else if (std::strcmp(argv[i], "--output-dir") == 0) outputDir = next();
        else if (std::strcmp(argv[i], "--count") == 0) count = std::stoi(next());
        else if (std::strcmp(argv[i], "--seed") == 0) seed = std::stoul(next());
        else if (std::strcmp(argv[i], "--threads") == 0) numThreads = std::stoi(next());
        else if (std::strcmp(argv[i], "--memory-limit-mb") == 0) memoryLimitMB = std::stoul(next());
        else if (std::strcmp(argv[i], "--io-backend") == 0) ioBackendStr = next();
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) { printUsage(argv[0]); return 0; }
        else { std::cerr << "Unknown option: " << argv[i] << "\n"; printUsage(argv[0]); return 1; }
    }

    if (inputPath.empty() || axisStr.empty() || outputDir.empty()) {
        std::cerr << "Error: --input, --axis, --output-dir are required\n";
        printUsage(argv[0]);
        return 1;
    }
    if (count < 1) {
        std::cerr << "Error: --count must be >= 1\n";
        return 1;
    }

    std::error_code ec;
    std::filesystem::create_directories(outputDir, ec);
    if (ec) {
        std::cerr << "Error: " << ec.message() << "\n";
        return 1;
    }

    erwt3d::ERWT3DReader reader(inputPath);
    if (ioBackendStr == "sb" || ioBackendStr == "superblock") {
        reader.setIOBackend(erwt3d::IOBackend::Superblock);
    } else if (ioBackendStr != "pread") {
        std::cerr << "Error: unknown --io-backend: " << ioBackendStr << " (valid: pread, sb)\n";
        return 1;
    }

    const auto& h = reader.getHeader();
    erwt3d::SliceAxis axis;
    uint64_t lineLen = 0;
    uint64_t maxFixed1 = 0;
    uint64_t maxFixed2 = 0;
    if (axisStr == "x" || axisStr == "X") {
        axis = erwt3d::SliceAxis::X;
        lineLen = h.nx;
        maxFixed1 = h.ny;
        maxFixed2 = h.nz;
    } else if (axisStr == "y" || axisStr == "Y") {
        axis = erwt3d::SliceAxis::Y;
        lineLen = h.ny;
        maxFixed1 = h.nx;
        maxFixed2 = h.nz;
    } else if (axisStr == "z" || axisStr == "Z") {
        axis = erwt3d::SliceAxis::Z;
        lineLen = h.nz;
        maxFixed1 = h.nx;
        maxFixed2 = h.ny;
    } else {
        std::cerr << "Error: invalid axis: " << axisStr << "\n";
        return 1;
    }

    std::mt19937 rng(seed);
    std::uniform_int_distribution<uint64_t> dist1(0, maxFixed1 - 1);
    std::uniform_int_distribution<uint64_t> dist2(0, maxFixed2 - 1);
    std::vector<float> output(lineLen);
    std::vector<double> timesMs;
    timesMs.reserve(count);
    uint64_t totalOutputBytes = 0;

    std::string csvPath = outputDir + "/line_benchmark.csv";
    std::ofstream csv(csvPath);
    csv << "iteration,axis,fixed1,fixed2,time_ms,output_bytes,output_file\n";

    for (int i = 0; i < count; ++i) {
        uint64_t fixed1 = dist1(rng);
        uint64_t fixed2 = dist2(rng);
        std::string outPath = outputDir + "/" + axisStr + "_line_" + std::to_string(i) + ".raw";

        auto start = std::chrono::high_resolution_clock::now();
        if (!reader.readLine(axis, fixed1, fixed2, output.data(), numThreads, memoryLimitMB)) {
            std::cerr << "Error: line read failed at iteration " << i << "\n";
            return 1;
        }
        if (!writeRaw(outPath, output.data(), lineLen * sizeof(float))) {
            std::cerr << "Error: write failed for " << outPath << "\n";
            return 1;
        }
        auto end = std::chrono::high_resolution_clock::now();

        double elapsedMs = std::chrono::duration<double, std::milli>(end - start).count();
        timesMs.push_back(elapsedMs);
        totalOutputBytes += lineLen * sizeof(float);
        csv << i << "," << axisStr << "," << fixed1 << "," << fixed2 << ","
            << std::fixed << std::setprecision(3) << elapsedMs << "," << (lineLen * sizeof(float))
            << "," << outPath << "\n";
    }
    csv.close();

    double totalMs = 0.0;
    for (double t : timesMs) totalMs += t;
    double minMs = *std::min_element(timesMs.begin(), timesMs.end());
    double maxMs = *std::max_element(timesMs.begin(), timesMs.end());
    double meanMs = totalMs / static_cast<double>(timesMs.size());
    double medianMs = medianOf(timesMs);

    std::string summaryPath = outputDir + "/line_summary.csv";
    std::ofstream summary(summaryPath);
    summary << "metric,value\n"
            << "axis," << axisStr << "\n"
            << "count," << count << "\n"
            << "seed," << seed << "\n"
            << "threads," << numThreads << "\n"
            << "memory_limit_mb," << memoryLimitMB << "\n"
            << "io_backend," << ioBackendStr << "\n"
            << std::fixed << std::setprecision(3)
            << "total_time_ms," << totalMs << "\n"
            << "avg_time_ms," << meanMs << "\n"
            << "median_time_ms," << medianMs << "\n"
            << "min_time_ms," << minMs << "\n"
            << "max_time_ms," << maxMs << "\n"
            << "output_bytes_total," << totalOutputBytes << "\n";
    summary.close();

    std::cout << "input: " << inputPath << "\n"
              << "axis: " << axisStr << "\n"
              << "count: " << count << "\n"
              << "seed: " << seed << "\n"
              << "threads: " << numThreads << "\n"
              << "memory_limit_mb: " << memoryLimitMB << "\n"
              << "io_backend: " << ioBackendStr << "\n"
              << "line_length: " << lineLen << "\n"
              << "total_time_ms: " << std::fixed << std::setprecision(3) << totalMs << "\n"
              << "avg_time_ms: " << meanMs << "\n"
              << "median_time_ms: " << medianMs << "\n"
              << "min_time_ms: " << minMs << "\n"
              << "max_time_ms: " << maxMs << "\n"
              << "output_bytes_total: " << totalOutputBytes << "\n"
              << "csv: " << csvPath << "\n"
              << "summary_csv: " << summaryPath << "\n";
    return 0;
}
