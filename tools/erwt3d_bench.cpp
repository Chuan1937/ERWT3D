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
#include <filesystem>
#include <sstream>
#include <functional>
#include <thread>

struct BenchmarkResult {
    std::string method;
    std::string ioBackend;
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
    file << "method,io_backend,axis,mode,count,avg_time_ms,min_time_ms,max_time_ms,total_time_ms,output_bytes" << std::endl;
    for (const auto& r : results) {
        file << r.method << ","
             << r.ioBackend << ","
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
    std::cerr << "  --mode MODE           Benchmark mode: normal, contest (default: normal)" << std::endl;
    std::cerr << "  --random-count N      Number of random slice reads per axis (default: 100)" << std::endl;
    std::cerr << "  --continuous-count N  Number of continuous slice reads per axis (default: 10)" << std::endl;
    std::cerr << "  --threads N|auto    Number of threads; auto = min(hw/2, 8) (default: 1)" << std::endl;
    std::cerr << "  --memory-limit-mb N   Memory limit in MB (default: 2048)" << std::endl;
    std::cerr << "  --cache-mb N          Cache size in MB (default: 0)" << std::endl;
    std::cerr << "  --io-backend MODE     I/O backend: pread, sb (default: pread)" << std::endl;
    std::cerr << "  --sb-parallel-mode M  SB parallel mode: serial, parallel-read (default: serial)" << std::endl;
    std::cerr << "  --sb-schedule MODE    SB task schedule: static, dynamic (default: static)" << std::endl;
    std::cerr << "  --sb-read-mode MODE   SB read mode: pread, run-batch, leaf-index, hdd-read-window (default: pread)" << std::endl;
    std::cerr << "  --leaf-merge-bytes N   Leaf-index merge extent size (default: 4096)" << std::endl;
    std::cerr << "  --sb-task-order MODE  SB task order: logical, file-offset (default: logical)" << std::endl;
    std::cerr << "  --hdd-read-window-bytes N  HDD read window max bytes (0=disabled, default: 0)" << std::endl;
    std::cerr << "  --hdd-max-gap-bytes N      HDD max gap bytes to merge (0=adjacent only, default: 0)" << std::endl;
    std::cerr << "  --hdd-batch-planner on|off  Global task sort + merge across all slices (default: off)" << std::endl;
    std::cerr << "  --hdd-batch-window-bytes N  Batch read window max bytes (0=auto, default: 0)" << std::endl;
    std::cerr << "  --hdd-batch-window-bytes N  Batch read window max bytes (0=auto, default: 0)" << std::endl;
    std::cerr << "  --hdd-batch-max-gap-bytes N Batch max gap bytes to merge (0=adjacent only, default: 0)" << std::endl;
    std::cerr << "  --hdd-contiguous-prefetch N Contiguous slice prefetch count (0=off, default: 0)" << std::endl;
    std::cerr << "  --profile-io          Enable per-slice I/O phase profiling (writes io_profile.csv)" << std::endl;
    std::cerr << "  --pin-threads         Pin worker threads to CPU cores (Linux only)" << std::endl;
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
    std::string ioBackendStr = "pread";
    std::string sbParallelModeStr = "serial";
    std::string sbScheduleStr = "static";
    std::string sbReadModeStr = "pread";
    size_t leafMergeBytes = 16384;
    std::string sbTaskOrderStr = "logical";
    uint64_t hddReadWindowBytes = 0;
    uint64_t hddMaxGapBytes = 0;
    bool hddBatchPlanner = false;
    uint64_t hddBatchWindowBytes = 0;
    uint64_t hddBatchMaxGapBytes = 0;
    uint32_t hddContiguousPrefetch = 0;
    std::string benchMode = "normal";
    bool profileIO = false;
    bool pinThreads = false;
    
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
            std::string ts = argv[++i];
            if (ts == "auto") {
                unsigned hw = std::thread::hardware_concurrency();
                numThreads = static_cast<int>(std::min(std::max(1u, hw / 2), 8u));
                std::cout << "Auto threads: " << numThreads << " (hw=" << hw << ", capped at 8)" << std::endl;
            } else {
                numThreads = std::stoi(ts);
            }
        } else if (std::strcmp(argv[i], "--memory-limit-mb") == 0 && i + 1 < argc) {
            memoryLimitMB = std::stoul(argv[++i]);
        } else if (std::strcmp(argv[i], "--cache-mb") == 0 && i + 1 < argc) {
            cacheMB = std::stoul(argv[++i]);
        } else if (std::strcmp(argv[i], "--io-backend") == 0 && i + 1 < argc) {
            ioBackendStr = argv[++i];
        } else if (std::strcmp(argv[i], "--sb-parallel-mode") == 0 && i + 1 < argc) {
            sbParallelModeStr = argv[++i];
        } else if (std::strcmp(argv[i], "--sb-schedule") == 0 && i + 1 < argc) {
            sbScheduleStr = argv[++i];
        } else if (std::strcmp(argv[i], "--sb-read-mode") == 0 && i + 1 < argc) {
            sbReadModeStr = argv[++i];
        } else if (std::strcmp(argv[i], "--leaf-merge-bytes") == 0 && i + 1 < argc) {
            leafMergeBytes = std::stoul(argv[++i]);
        } else if (std::strcmp(argv[i], "--sb-task-order") == 0 && i + 1 < argc) {
            sbTaskOrderStr = argv[++i];
        } else if (std::strcmp(argv[i], "--hdd-read-window-bytes") == 0 && i + 1 < argc) {
            hddReadWindowBytes = std::stoul(argv[++i]);
        } else if (std::strcmp(argv[i], "--hdd-max-gap-bytes") == 0 && i + 1 < argc) {
            hddMaxGapBytes = std::stoul(argv[++i]);
        } else if (std::strcmp(argv[i], "--hdd-batch-planner") == 0 && i + 1 < argc) {
            hddBatchPlanner = (std::strcmp(argv[++i], "on") == 0);
        } else if (std::strcmp(argv[i], "--hdd-batch-window-bytes") == 0 && i + 1 < argc) {
            hddBatchWindowBytes = std::stoul(argv[++i]);
        } else if (std::strcmp(argv[i], "--hdd-batch-max-gap-bytes") == 0 && i + 1 < argc) {
            hddBatchMaxGapBytes = std::stoul(argv[++i]);
        } else if (std::strcmp(argv[i], "--hdd-contiguous-prefetch") == 0 && i + 1 < argc) {
            hddContiguousPrefetch = std::stoul(argv[++i]);
        } else if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            benchMode = argv[++i];
        } else if (std::strcmp(argv[i], "--profile-io") == 0) {
            profileIO = true;
        } else if (std::strcmp(argv[i], "--pin-threads") == 0) {
            pinThreads = true;
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
    {
        std::error_code ec;
        std::filesystem::create_directories(outputDir, ec);
        if (ec) {
            std::cerr << "Error: Cannot create output directory: " << ec.message() << std::endl;
            return 1;
        }
    }
    
    // Open reader
    erwt3d::ERWT3DReader reader(inputPath, cacheMB);
    if (ioBackendStr == "pread") {
        // default
    } else if (ioBackendStr == "sb" || ioBackendStr == "superblock") {
        reader.setIOBackend(erwt3d::IOBackend::Superblock);
    } else {
        std::cerr << "Error: Unknown --io-backend: " << ioBackendStr << " (valid: pread, sb)" << std::endl;
        return 1;
    }
    
    if (sbParallelModeStr == "serial") {
        reader.setSBParallelMode(erwt3d::SBParallelMode::Serial);
    } else if (sbParallelModeStr == "parallel-read") {
        reader.setSBParallelMode(erwt3d::SBParallelMode::ParallelRead);
    } else {
        std::cerr << "Error: Unknown --sb-parallel-mode: " << sbParallelModeStr << " (valid: serial, parallel-read)" << std::endl;
        return 1;
    }
    
    if (sbScheduleStr == "dynamic") {
        reader.setSBSchedule(erwt3d::SBSchedule::Dynamic);
    } else if (sbScheduleStr != "static") {
        std::cerr << "Error: Unknown --sb-schedule: " << sbScheduleStr << " (valid: static, dynamic)" << std::endl;
        return 1;
    }
    reader.setPinThreads(pinThreads);
    
    if (sbReadModeStr == "run-batch") {
        reader.setSBReadMode(erwt3d::SBReadMode::RunBatch);
    } else if (sbReadModeStr == "leaf-index") {
        reader.setSBReadMode(erwt3d::SBReadMode::LeafIndex);
        reader.setLeafMergeBytes(leafMergeBytes);
    } else if (sbReadModeStr == "hdd-read-window") {
        reader.setSBReadMode(erwt3d::SBReadMode::HDDReadWindow);
        erwt3d::HDDReadWindowConfig cfg;
        cfg.read_window_bytes = hddReadWindowBytes;
        cfg.max_gap_bytes = hddMaxGapBytes;
        reader.setHDDReadWindowConfig(cfg);
    } else if (sbReadModeStr != "pread") {
        std::cerr << "Error: Unknown --sb-read-mode: " << sbReadModeStr << " (valid: pread, run-batch, leaf-index, hdd-read-window)" << std::endl;
        return 1;
    }
    
    if (sbTaskOrderStr == "file-offset") {
        reader.setSBTaskOrder(erwt3d::SBTaskOrder::FileOffset);
    } else if (sbTaskOrderStr != "logical") {
        std::cerr << "Error: Unknown --sb-task-order: " << sbTaskOrderStr << " (valid: logical, file-offset)" << std::endl;
        return 1;
    }
    reader.setProfileIO(profileIO);
    const auto& header = reader.getHeader();
    
    std::cout << "ERWT3D Benchmark" << std::endl;
    std::cout << "===============" << std::endl;
    std::cout << "Dimensions: " << header.nx << " x " << header.ny << " x " << header.nz << std::endl;
    std::cout << "Random slices: " << randomCount << " per axis" << std::endl;
    std::cout << "Continuous slices: " << continuousCount << " per axis" << std::endl;
    std::cout << "Threads: " << numThreads << std::endl;
    std::cout << "Memory limit: " << memoryLimitMB << " MB" << std::endl;
    std::cout << "Cache: " << cacheMB << " MB" << std::endl;
    std::cout << "IO backend: " << ioBackendStr << std::endl;
    if (ioBackendStr == "sb" || ioBackendStr == "superblock") {
        std::cout << "SB parallel mode: " << sbParallelModeStr << std::endl;
        std::cout << "SB schedule: " << sbScheduleStr << std::endl;
        std::cout << "SB read mode: " << sbReadModeStr << std::endl;
        std::cout << "SB task order: " << sbTaskOrderStr << std::endl;
        if (sbReadModeStr == "hdd-read-window") {
            std::cout << "HDD read window bytes: " << hddReadWindowBytes << std::endl;
            std::cout << "HDD max gap bytes: " << hddMaxGapBytes << std::endl;
        }
        if (hddBatchPlanner) {
            std::cout << "HDD batch planner: ON" << std::endl;
            std::cout << "HDD batch window bytes: " << hddBatchWindowBytes << std::endl;
            std::cout << "HDD batch max gap bytes: " << hddBatchMaxGapBytes << std::endl;
        }
    }
    if (profileIO) {
        std::cout << "Profile IO: enabled" << std::endl;
    }
    std::cout << std::endl;
    
    std::vector<BenchmarkResult> results;
    std::vector<std::string> detailLines;  // per-slice detail for bench_detail.csv
    std::vector<std::string> profileLines; // per-slice phase timing for io_profile.csv
    std::mt19937 rng(seed);
    bool benchOk = true;
    
    // Benchmark function
    auto benchmarkSlice = [&](erwt3d::SliceAxis axis, const std::string& axisName, 
                                const std::vector<uint64_t>& indices, const std::string& mode) -> bool {
        std::vector<double> times;
        std::vector<uint64_t> detailIndices;
        std::vector<double> detailTimes;
        uint64_t outputBytes = 0;
        
        std::cout << "  " << axisName << " " << mode << " (" << indices.size() << " slices):" << std::endl;
        
        for (size_t i = 0; i < indices.size(); ++i) {
            if (i % std::max(size_t(1), indices.size() / 10) == 0 || i == indices.size() - 1) {
                std::cout << "    [" << (i+1) << "/" << indices.size() << "]" << std::flush;
            }
            uint64_t idx = indices[i];
            
            // Calculate output size
            uint64_t sliceSize;
            switch (axis) {
                case erwt3d::SliceAxis::X: sliceSize = header.ny * header.nz; break;
                case erwt3d::SliceAxis::Y: sliceSize = header.nx * header.nz; break;
                case erwt3d::SliceAxis::Z: sliceSize = header.nx * header.ny; break;
            }
            
            std::vector<float> output(sliceSize);
            
            std::string outPath = outputDir + "/" + axisName + "_" + mode + "_" + std::to_string(i) + ".raw";
            
            auto start = std::chrono::high_resolution_clock::now();
            
            if (!reader.readSlice(axis, idx, output.data(), numThreads, memoryLimitMB)) {
                std::cerr << "Error: Failed to read slice" << std::endl;
                return false;
            }
            
            auto readEnd = std::chrono::high_resolution_clock::now();
            double readMs = std::chrono::duration<double, std::milli>(readEnd - start).count();

            // Write to file
            std::ofstream outFile(outPath, std::ios::binary);
            if (!outFile) {
                std::cerr << "Error: Cannot open output file: " << outPath << std::endl;
                return false;
            }
            outFile.write(reinterpret_cast<const char*>(output.data()), sliceSize * sizeof(float));
            outFile.close();
            if (!outFile.good()) {
                std::cerr << "Error: Failed to write output file: " << outPath << std::endl;
                return false;
            }
            
            auto end = std::chrono::high_resolution_clock::now();
            double timeMs = std::chrono::duration<double, std::milli>(end - start).count();
            double writeMs = timeMs - readMs;
            
            times.push_back(timeMs);
            detailIndices.push_back(idx);
            detailTimes.push_back(timeMs);
            
            // Append to detail CSV lines
            std::ostringstream dl;
            dl << axisName << "," << mode << "," << i << "," << idx << ","
               << std::fixed << std::setprecision(3) << timeMs << ","
               << (sliceSize * sizeof(float)) << ","
               << ioBackendStr << "," << numThreads << "," << cacheMB << "," << memoryLimitMB
               << "," << sbParallelModeStr;
            detailLines.push_back(dl.str());
            
            if (profileIO) {
                const auto& p = reader.lastProfile();
                std::ostringstream pl;
                pl << axisName << "," << mode << "," << idx << ","
                   << ioBackendStr << "," << numThreads << "," << sbParallelModeStr << ","
                   << p.superblocks_touched << "," << p.pread_calls << "," << p.bytes_read << ","
                   << p.output_bytes << ","
                   << std::fixed << std::setprecision(3) << p.plan_time_ms << ","
                   << p.read_time_ms << "," << p.unpack_time_ms << ","
                   << p.read_time_sum_ms << "," << p.unpack_time_sum_ms << ","
                   << (p.panel_hit ? "true" : "false") << ","
                   << readMs << "," << writeMs;
                profileLines.push_back(pl.str());
            }
            
            outputBytes = sliceSize * sizeof(float);
        }
        
        // Calculate statistics
        double totalTime = std::accumulate(times.begin(), times.end(), 0.0);
        double avgTime = totalTime / times.size();
        double minTime = *std::min_element(times.begin(), times.end());
        double maxTime = *std::max_element(times.begin(), times.end());
        
        BenchmarkResult result;
        result.method = "erwt3d";
        result.ioBackend = ioBackendStr;
        result.axis = axisName;
        result.mode = mode;
        result.count = indices.size();
        result.avgTimeMs = avgTime;
        result.minTimeMs = minTime;
        result.maxTimeMs = maxTime;
        result.totalTimeMs = totalTime;
        result.outputBytes = outputBytes;
        
        results.push_back(result);
        
        std::cout << "\r  " << axisName << " " << mode << ": avg=" << std::fixed << std::setprecision(2) 
                  << avgTime << "ms, min=" << minTime << "ms, max=" << maxTime << "ms" << std::endl;
        
        return true;
    };

    // Batch planner benchmark: collect all slices, global sort, one read pass
    auto benchmarkSliceBatch = [&](erwt3d::SliceAxis axis, const std::string& axisName,
                                    const std::vector<uint64_t>& indices, const std::string& mode) -> bool {
        uint64_t sliceSize;
        switch (axis) {
            case erwt3d::SliceAxis::X: sliceSize = header.ny * header.nz; break;
            case erwt3d::SliceAxis::Y: sliceSize = header.nx * header.nz; break;
            case erwt3d::SliceAxis::Z: sliceSize = header.nx * header.ny; break;
        }
        uint64_t outBytes = sliceSize * sizeof(float);
        std::cout << "  " << axisName << " " << mode << " (" << indices.size() << " slices, batch):" << std::endl;
        std::vector<std::vector<float>> outputs(indices.size());
        std::vector<erwt3d::ERWT3DReader::SliceBatchRequest> reqs;
        for (size_t i = 0; i < indices.size(); ++i) {
            outputs[i].resize(sliceSize);
            reqs.push_back({axis, indices[i], outputs[i].data()});
        }
        erwt3d::HDDReadWindowConfig bwcfg{hddBatchWindowBytes, hddBatchMaxGapBytes};
        auto t0 = std::chrono::high_resolution_clock::now();
        if (!reader.readSlicesBatch(reqs, numThreads, memoryLimitMB, bwcfg)) {
            std::cerr << "Error: batch read failed" << std::endl; return false;
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        double totalMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        double avgMs = totalMs / indices.size();
        for (size_t i = 0; i < indices.size(); ++i) {
            std::string op = outputDir + "/" + axisName + "_" + mode + "_" + std::to_string(i) + ".raw";
            std::ofstream of(op, std::ios::binary);
            of.write(reinterpret_cast<const char*>(outputs[i].data()), outBytes); of.close();
            std::ostringstream dl;
            dl << axisName << "," << mode << "," << i << "," << indices[i] << ","
               << std::fixed << std::setprecision(3) << avgMs << "," << outBytes << ","
               << ioBackendStr << "," << numThreads << "," << cacheMB << "," << memoryLimitMB
               << "," << sbParallelModeStr;
            detailLines.push_back(dl.str());
        }
        BenchmarkResult r; r.method = "erwt3d"; r.ioBackend = ioBackendStr;
        r.axis = axisName; r.mode = mode; r.count = indices.size();
        r.avgTimeMs = avgMs; r.minTimeMs = avgMs; r.maxTimeMs = avgMs;
        r.totalTimeMs = totalMs; r.outputBytes = outBytes;
        results.push_back(r);
        std::cout << "  " << axisName << " " << mode << ": batch_avg=" << std::fixed
                  << std::setprecision(2) << avgMs << "ms, batch_total=" << totalMs << "ms" << std::endl;
        return true;
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
    
    // Generate continuous indices (safe against underflow and overflow)
    auto safeStart = [](uint64_t dim, int cnt) -> uint64_t {
        if (static_cast<uint64_t>(cnt) >= dim) return 0;
        return dim / 2 - cnt / 2;
    };
    std::vector<uint64_t> continuousX, continuousY, continuousZ;
    uint64_t sX = safeStart(header.nx, continuousCount);
    uint64_t sY = safeStart(header.ny, continuousCount);
    uint64_t sZ = safeStart(header.nz, continuousCount);
    int countX = std::min(continuousCount, static_cast<int>(header.nx));
    int countY = std::min(continuousCount, static_cast<int>(header.ny));
    int countZ = std::min(continuousCount, static_cast<int>(header.nz));
    for (int i = 0; i < countX; ++i) continuousX.push_back(sX + i);
    for (int i = 0; i < countY; ++i) continuousY.push_back(sY + i);
    for (int i = 0; i < countZ; ++i) continuousZ.push_back(sZ + i);
    
    // Contest mode: global all-axis batch, includes write time
    auto benchmarkContest = [&]() -> bool {
        uint64_t sx = header.ny * header.nz, sy = header.nx * header.nz, sz = header.nx * header.ny;
        std::cout << "Contest: " << (randomCount*3+countX+countY+countZ) << " slices global batch" << std::endl;
        using R = erwt3d::ERWT3DReader::SliceBatchRequest;
        std::vector<R> reqs;
        for (int i=0;i<randomCount;++i) reqs.push_back(R{erwt3d::SliceAxis::X,randomX[i],nullptr});
        for (int i=0;i<randomCount;++i) reqs.push_back(R{erwt3d::SliceAxis::Y,randomY[i],nullptr});
        for (int i=0;i<randomCount;++i) reqs.push_back(R{erwt3d::SliceAxis::Z,randomZ[i],nullptr});
        for (int i=0;i<countX;++i) reqs.push_back(R{erwt3d::SliceAxis::X,continuousX[i],nullptr});
        for (int i=0;i<countY;++i) reqs.push_back(R{erwt3d::SliceAxis::Y,continuousY[i],nullptr});
        for (int i=0;i<countZ;++i) reqs.push_back(R{erwt3d::SliceAxis::Z,continuousZ[i],nullptr});

        auto obf=[&](erwt3d::SliceAxis a){return (a==erwt3d::SliceAxis::X?sx:(a==erwt3d::SliceAxis::Y?sy:sz))*sizeof(float);};
        std::vector<std::vector<float>> bufs(reqs.size());
        for(size_t i=0;i<reqs.size();++i){auto a=reqs[i].axis;bufs[i].resize(a==erwt3d::SliceAxis::X?sx:a==erwt3d::SliceAxis::Y?sy:sz);reqs[i].output=bufs[i].data();}

        auto t0=std::chrono::high_resolution_clock::now();
        if(!reader.readSlicesBatch(reqs,numThreads,memoryLimitMB,{hddBatchWindowBytes,hddBatchMaxGapBytes})){
            std::cerr<<"Contest batch read failed\n";return false;}
        auto tr=std::chrono::high_resolution_clock::now();
        double readMs=std::chrono::duration<double,std::milli>(tr-t0).count();

        // Write all outputs
        size_t ri=0;
        auto wp=[&](erwt3d::SliceAxis ax,const std::string& an,int cnt,const std::string& md){
            for(int i=0;i<cnt;++i,++ri){std::string op=outputDir+"/"+an+"_"+md+"_"+std::to_string(i)+".raw";
            std::ofstream of(op,std::ios::binary);of.write((const char*)bufs[ri].data(),obf(ax));of.close();}};
        wp(erwt3d::SliceAxis::X,"x",randomCount,"random");
        wp(erwt3d::SliceAxis::Y,"y",randomCount,"random");
        wp(erwt3d::SliceAxis::Z,"z",randomCount,"random");
        wp(erwt3d::SliceAxis::X,"x",countX,"continuous");
        wp(erwt3d::SliceAxis::Y,"y",countY,"continuous");
        wp(erwt3d::SliceAxis::Z,"z",countZ,"continuous");

        auto t1=std::chrono::high_resolution_clock::now();
        double totalMs=std::chrono::duration<double,std::milli>(t1-t0).count();
        double avgMs=totalMs/reqs.size();
        std::cout<<"Contest: read="<<std::fixed<<std::setprecision(0)<<readMs
                 <<"ms write="<<(totalMs-readMs)<<"ms total="<<totalMs
                 <<"ms per_slice="<<std::setprecision(2)<<avgMs<<"ms\n";
        struct{const char*ax,*md;int cnt;}es[]={{"x","random",randomCount},{"y","random",randomCount},{"z","random",randomCount},{"x","continuous",countX},{"y","continuous",countY},{"z","continuous",countZ}};
        for(auto&e:es){BenchmarkResult r;r.method="erwt3d";r.ioBackend="sb-contest";r.axis=e.ax;r.mode=e.md;r.count=e.cnt;r.avgTimeMs=avgMs;r.minTimeMs=avgMs;r.maxTimeMs=avgMs;r.totalTimeMs=totalMs;r.outputBytes=(e.ax[0]=='x'?sx*sizeof(float):e.ax[0]=='y'?sy*sizeof(float):sz*sizeof(float));results.push_back(r);
        for(int i=0;i<e.cnt;++i){std::ostringstream dl;dl<<e.ax<<","<<e.md<<","<<i<<","<<i<<","<<std::fixed<<std::setprecision(3)<<avgMs<<","<<r.outputBytes<<",sb-contest,"<<numThreads<<","<<cacheMB<<","<<memoryLimitMB<<","<<sbParallelModeStr;detailLines.push_back(dl.str());}}
        return true;
    };
    
    // Display mode
    std::cout << "Benchmark mode: " << benchMode << std::endl;
    std::cout << "HDD batch planner: " << (hddBatchPlanner?"ON":"OFF") << std::endl;

    // Run benchmarks
    if (benchMode == "contest") {
        std::cout << "\nRunning contest benchmark [GLOBAL BATCH, ALL AXES]..." << std::endl;
        benchmarkContest() || (benchOk = false);
    } else {
        std::function<bool(erwt3d::SliceAxis,const std::string&,const std::vector<uint64_t>&,const std::string&)> bn;
        bn = hddBatchPlanner ? (decltype(bn))benchmarkSliceBatch : (decltype(bn))benchmarkSlice;
        std::cout << "\nRunning random slice benchmarks..." << (hddBatchPlanner ? " [BATCH]" : "") << std::endl;
        bn(erwt3d::SliceAxis::X, "x", randomX, "random") || (benchOk = false);
        bn(erwt3d::SliceAxis::Y, "y", randomY, "random") || (benchOk = false);
        bn(erwt3d::SliceAxis::Z, "z", randomZ, "random") || (benchOk = false);
        std::cout << "\nRunning continuous slice benchmarks..." << (hddBatchPlanner ? " [BATCH]" : "") << std::endl;
        bn(erwt3d::SliceAxis::X, "x", continuousX, "continuous") || (benchOk = false);
        bn(erwt3d::SliceAxis::Y, "y", continuousY, "continuous") || (benchOk = false);
        bn(erwt3d::SliceAxis::Z, "z", continuousZ, "continuous") || (benchOk = false);
    }
    
    // Write CSV
    if (!benchOk) {
        std::cerr << "Warning: benchmark completed with errors" << std::endl;
        return 1;
    }
    
    std::string csvPath = outputDir + "/bench_result.csv";
    {
        std::ofstream cf(csvPath);
        if (!cf) { std::cerr << "Error: Cannot write " << csvPath << std::endl; return 1; }
        cf << "method,io_backend,sb_parallel_mode,axis,mode,count,avg_time_ms,min_time_ms,max_time_ms,total_time_ms,output_bytes" << std::endl;
        for (const auto& r : results) {
            cf << r.method << "," << r.ioBackend << "," << sbParallelModeStr << ","
               << r.axis << "," << r.mode << "," << r.count << ","
               << std::fixed << std::setprecision(3) << r.avgTimeMs << ","
               << r.minTimeMs << "," << r.maxTimeMs << "," << r.totalTimeMs << ","
               << r.outputBytes << std::endl;
        }
        cf.close();
        if (!cf.good()) { std::cerr << "Error: Failed to write " << csvPath << std::endl; return 1; }
    }
    std::cout << "\nResults written to " << csvPath << std::endl;
    
    // Write detail CSV
    std::string detailPath = outputDir + "/bench_detail.csv";
    {
        std::ofstream df(detailPath);
        if (!df) { std::cerr << "Error: Cannot write " << detailPath << std::endl; return 1; }
        df << "axis,mode,iteration,index,time_ms,output_bytes,io_backend,threads,cache_mb,memory_limit_mb,sb_parallel_mode" << std::endl;
        for (const auto& line : detailLines) df << line << std::endl;
        df.close();
        if (!df.good()) { std::cerr << "Error: Failed to write " << detailPath << std::endl; return 1; }
    }
    std::cout << "Detail results written to " << detailPath << std::endl;
    
    if (profileIO && !profileLines.empty()) {
        std::string profilePath = outputDir + "/io_profile.csv";
        std::ofstream pf(profilePath);
        if (!pf) { std::cerr << "Error: Cannot write " << profilePath << std::endl; return 1; }
        pf << "axis,mode,index,backend,threads,sb_parallel_mode,superblocks_touched,pread_calls,bytes_read,output_bytes,plan_time_ms,read_time_wall_ms,unpack_time_wall_ms,read_time_sum_ms,unpack_time_sum_ms,panel_hit,read_slice_ms,write_output_ms" << std::endl;
        for (const auto& line : profileLines) pf << line << std::endl;
        pf.close();
        std::cout << "IO profile written to " << profilePath << std::endl;
    }
    
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