#include "erwt3d/file_format_detect.hpp"
#include "erwt3d/io_profile.hpp"
#include "erwt3d/memory_budget.hpp"
#include "erwt3d/reader.hpp"
#include "erwt3d/rzfp_reader.hpp"
#include "erwt3d/unified_read_config.hpp"

#include <chrono>
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

double elapsedMs(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string jsonEscape(const std::string& value) {
    std::ostringstream out;
    for (unsigned char c : value) {
        switch (c) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                                  << static_cast<unsigned>(c) << std::dec << std::setfill(' ');
                else out << c;
        }
    }
    return out.str();
}

bool mkdirP(const std::string& path) {
    if (path.empty()) return false;
    std::string current = path.front() == '/' ? "/" : "";
    size_t start = 0;
    while (start < path.size()) {
        const size_t end = path.find('/', start);
        const std::string part = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!part.empty()) {
            if (!current.empty() && current.back() != '/') current.push_back('/');
            current += part;
            if (mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

bool writeFile(const std::string& path, const float* data, size_t bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(bytes));
    return static_cast<bool>(output);
}

erwt3d::SliceAxis parseAxis(const std::string& axis) {
    if (axis == "x" || axis == "X") return erwt3d::SliceAxis::X;
    if (axis == "y" || axis == "Y") return erwt3d::SliceAxis::Y;
    if (axis == "z" || axis == "Z") return erwt3d::SliceAxis::Z;
    throw std::invalid_argument("axis must be x, y, or z");
}

std::vector<uint64_t> loadPositions(const std::string& path, uint64_t bound,
                                    bool continuous) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open positions file " + path);
    std::vector<uint64_t> positions;
    std::set<uint64_t> seen;
    std::string line;
    while (std::getline(input, line)) {
        const auto comment = line.find('#');
        if (comment != std::string::npos) line.resize(comment);
        const auto begin = line.find_first_not_of(" \t\r");
        if (begin == std::string::npos) continue;
        const auto end = line.find_last_not_of(" \t\r");
        const std::string token = line.substr(begin, end - begin + 1);
        size_t parsed = 0;
        const uint64_t value = std::stoull(token, &parsed);
        if (parsed != token.size() || value >= bound) throw std::runtime_error("invalid position " + token);
        if (!seen.insert(value).second) throw std::runtime_error("duplicate position " + token);
        positions.push_back(value);
    }
    if (positions.empty()) throw std::runtime_error("positions file is empty");
    if (continuous) {
        for (size_t i = 1; i < positions.size(); ++i) {
            if (positions[i] != positions[i - 1] + 1) {
                throw std::runtime_error("continuous positions must be strictly consecutive in file order");
            }
        }
    }
    return positions;
}

void usage(const char* program) {
    std::cerr << "Usage: " << program << " --input FILE --output-dir DIR --positions-file FILE "
              << "--axis x|y|z --pattern random|continuous --metrics-json FILE [options]\n"
              << "\nOne invocation measures exactly one axis and one access pattern. The positions file\n"
              << "contains one index per line; random order is preserved and continuous indices are validated.\n"
              << "\nOptions:\n"
              << "  --threads N                 Decode/read worker count (default: 8)\n"
              << "  --memory-limit-mb auto|N    Reader memory limit (default: auto)\n"
              << "  --io-profile auto|hdd|ssd|wsl-ssd\n"
              << "  --disable-access-planner    Benchmark-only generic LZ4 planning / RZFP selective strategy\n"
              << "  --disable-device-scheduling Benchmark-only generic profile (no device-specific scheduler)\n";
}
}

int main(int argc, char* argv[]) {
    std::string inputPath, outputDir, positionsPath, axisText, pattern, metricsPath;
    std::string memoryLimit = "auto", ioProfileText = "auto";
    int threads = 8;
    bool disablePlanner = false, disableDeviceScheduling = false;
    for (int i = 1; i < argc; ++i) {
        const auto next = [&]() -> const char* {
            if (i + 1 >= argc) throw std::runtime_error(std::string(argv[i]) + " requires a value");
            return argv[++i];
        };
        try {
            if (std::strcmp(argv[i], "--input") == 0) inputPath = next();
            else if (std::strcmp(argv[i], "--output-dir") == 0) outputDir = next();
            else if (std::strcmp(argv[i], "--positions-file") == 0) positionsPath = next();
            else if (std::strcmp(argv[i], "--axis") == 0) axisText = next();
            else if (std::strcmp(argv[i], "--pattern") == 0) pattern = next();
            else if (std::strcmp(argv[i], "--metrics-json") == 0) metricsPath = next();
            else if (std::strcmp(argv[i], "--threads") == 0) threads = std::stoi(next());
            else if (std::strcmp(argv[i], "--memory-limit-mb") == 0) memoryLimit = next();
            else if (std::strcmp(argv[i], "--io-profile") == 0) ioProfileText = next();
            else if (std::strcmp(argv[i], "--disable-access-planner") == 0) disablePlanner = true;
            else if (std::strcmp(argv[i], "--disable-device-scheduling") == 0) disableDeviceScheduling = true;
            else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) { usage(argv[0]); return 0; }
            else throw std::runtime_error(std::string("unknown option ") + argv[i]);
        } catch (const std::exception& error) {
            std::cerr << "Error: " << error.what() << "\n";
            return 1;
        }
    }
    try {
        if (inputPath.empty() || outputDir.empty() || positionsPath.empty() || axisText.empty() ||
            pattern.empty() || metricsPath.empty()) throw std::runtime_error("required options are missing");
        if (pattern != "random" && pattern != "continuous") throw std::runtime_error("pattern must be random or continuous");
        if (threads < 1) throw std::runtime_error("threads must be positive");
        if (!mkdirP(outputDir)) throw std::runtime_error("cannot create output directory " + outputDir);
        const auto axis = parseAxis(axisText);
        const auto format = erwt3d::detectOptimizedFileFormat(inputPath);
        if (format == erwt3d::OptimizedFileFormat::Unknown) throw std::runtime_error("input is not an ERWT3D file");
        const auto memory = erwt3d::resolveMemoryLimit(memoryLimit);
        if (!memory.valid) throw std::runtime_error(memory.error);
        uint64_t nx = 0, ny = 0, nz = 0;
        if (format == erwt3d::OptimizedFileFormat::LZ4_ERWT3D) {
            erwt3d::ERWT3DReader reader(inputPath);
            const auto& header = reader.getHeader(); nx = header.nx; ny = header.ny; nz = header.nz;
        } else {
            erwt3d::RzfpReader reader(inputPath);
            if (!reader.ok()) throw std::runtime_error("cannot open RZFP input");
            const auto& header = reader.header(); nx = header.nx; ny = header.ny; nz = header.nz;
        }
        const uint64_t axisLimit = axis == erwt3d::SliceAxis::X ? nx : axis == erwt3d::SliceAxis::Y ? ny : nz;
        const auto positions = loadPositions(positionsPath, axisLimit, pattern == "continuous");
        const uint64_t elements = axis == erwt3d::SliceAxis::X ? ny * nz : axis == erwt3d::SliceAxis::Y ? nx * nz : nx * ny;
        const uint64_t outputBytes = elements * sizeof(float);
        const auto requestedProfile = erwt3d::parseIOProfileType(ioProfileText);
        erwt3d::UnifiedReadConfig config = erwt3d::makeUnifiedConfig(requestedProfile, inputPath, threads, memory.mib, 0);
        if (disableDeviceScheduling) config = erwt3d::makeGenericReadConfig(threads, memory.mib);

        double apiReadMs = 0.0, writeMs = 0.0, createMs = 0.0;
        double decodeMs = 0.0, reorderMs = 0.0;
        bool detailedBreakdown = format == erwt3d::OptimizedFileFormat::RZFP;
        uint64_t bytesRead = 0, preadCalls = 0;
        std::vector<double> latencies, perRead, perWrite;
        const auto totalStart = Clock::now();

        std::unique_ptr<erwt3d::ERWT3DReader> lz4;
        std::unique_ptr<erwt3d::RzfpReader> rzfp;
        erwt3d::RzfpReaderConfig rzfpConfig;
        if (format == erwt3d::OptimizedFileFormat::LZ4_ERWT3D) {
            lz4 = std::make_unique<erwt3d::ERWT3DReader>(inputPath);
            lz4->setProfileIO(true);
            lz4->setIOBackend(erwt3d::IOBackend::Superblock);
            lz4->setSBTaskOrder(disablePlanner ? erwt3d::SBTaskOrder::Logical : erwt3d::SBTaskOrder::FileOffset);
            if (disablePlanner) lz4->setSBReadMode(erwt3d::SBReadMode::LeafIndex);
            else if (config.io_profile == erwt3d::IOProfileType::SSD || config.io_profile == erwt3d::IOProfileType::WSL_SSD) {
                lz4->setSBReadMode(erwt3d::SBReadMode::SSDConcurrentExtent); lz4->setSSDReadConfig(config.ssd);
            } else { lz4->setSBReadMode(erwt3d::SBReadMode::HDDReadWindow); lz4->setHDDReadWindowConfig(config.hdd); }
        } else {
            rzfp = std::make_unique<erwt3d::RzfpReader>(inputPath);
            rzfpConfig.io_profile = config.io_profile;
            rzfpConfig.decode_threads = threads;
            rzfpConfig.hdd = config.hdd;
            rzfpConfig.ssd = config.ssd;
            rzfpConfig.adaptive.auto_calibrate_device = !disableDeviceScheduling;
            rzfpConfig.strategy = disablePlanner ? erwt3d::RzfpReadStrategy::SelectiveLeaf : erwt3d::RzfpReadStrategy::Auto;
        }

        for (size_t i = 0; i < positions.size(); ++i) {
            const auto sliceStart = Clock::now();
            const auto createStart = Clock::now();
            std::vector<float> output(elements);
            const std::string outputPath = outputDir + "/paper_" + axisText + "_" + pattern + "_" +
                std::to_string(i) + ".dat";
            createMs += elapsedMs(createStart);
            const auto readStart = Clock::now();
            bool ok = false;
            if (lz4) {
                ok = lz4->readSlice(axis, positions[i], output.data(), threads, memory.mib);
                const auto& profile = lz4->lastProfile();
                bytesRead += profile.bytes_read; preadCalls += profile.pread_calls;
            } else {
                erwt3d::RzfpReadProfile profile;
                rzfpConfig.profile = &profile;
                ok = rzfp->readSlicesBatch({{axis, positions[i], output.data()}}, rzfpConfig);
                bytesRead += profile.actual_read_bytes; preadCalls += profile.pread_calls;
                decodeMs += profile.decode_time_ms; reorderMs += profile.scatter_time_ms;
            }
            const double oneReadMs = elapsedMs(readStart);
            apiReadMs += oneReadMs;
            if (!ok) throw std::runtime_error("reader failed at position " + std::to_string(positions[i]));
            const auto writeStart = Clock::now();
            if (!writeFile(outputPath, output.data(), outputBytes)) {
                throw std::runtime_error("cannot write output slice " + outputPath);
            }
            const double oneWriteMs = elapsedMs(writeStart);
            writeMs += oneWriteMs;
            latencies.push_back(elapsedMs(sliceStart)); perRead.push_back(oneReadMs); perWrite.push_back(oneWriteMs);
        }
        const double totalMs = elapsedMs(totalStart);
        std::ofstream metrics(metricsPath, std::ios::trunc);
        if (!metrics) throw std::runtime_error("cannot write metrics JSON " + metricsPath);
        metrics << std::fixed << std::setprecision(6);
        metrics << "{\n"
                << "  \"status\": \"SUCCESS\",\n"
                << "  \"input\": \"" << jsonEscape(inputPath) << "\",\n"
                << "  \"format\": \"" << (lz4 ? "lz4_erwt3d" : "rzfp") << "\",\n"
                << "  \"axis\": \"" << jsonEscape(axisText) << "\",\n"
                << "  \"pattern\": \"" << pattern << "\",\n"
                << "  \"positions\": [";
        for (size_t i = 0; i < positions.size(); ++i) metrics << (i ? ", " : "") << positions[i];
        metrics << "],\n  \"slice_latencies_ms\": [";
        for (size_t i = 0; i < latencies.size(); ++i) metrics << (i ? ", " : "") << latencies[i];
        metrics << "],\n  \"slice_read_latencies_ms\": [";
        for (size_t i = 0; i < perRead.size(); ++i) metrics << (i ? ", " : "") << perRead[i];
        metrics << "],\n  \"slice_write_latencies_ms\": [";
        for (size_t i = 0; i < perWrite.size(); ++i) metrics << (i ? ", " : "") << perWrite[i];
        metrics << "],\n"
                << "  \"read_time_ms\": " << apiReadMs << ",\n"
                << "  \"decode_time_ms\": ";
        if (detailedBreakdown) metrics << decodeMs; else metrics << "null";
        metrics << ",\n  \"reorder_time_ms\": ";
        if (detailedBreakdown) metrics << reorderMs; else metrics << "null";
        metrics << ",\n  \"write_time_ms\": " << writeMs << ",\n"
                << "  \"output_create_time_ms\": " << createMs << ",\n"
                << "  \"total_time_ms\": " << totalMs << ",\n"
                << "  \"bytes_read\": " << bytesRead << ",\n"
                << "  \"bytes_written\": " << outputBytes * positions.size() << ",\n"
                << "  \"pread_calls\": " << preadCalls << ",\n"
                << "  \"threads\": " << threads << ",\n"
                << "  \"io_profile\": \"" << erwt3d::ioProfileTypeName(config.io_profile) << "\",\n"
                << "  \"access_planner_disabled\": " << (disablePlanner ? "true" : "false") << ",\n"
                << "  \"device_scheduling_disabled\": " << (disableDeviceScheduling ? "true" : "false") << "\n}\n";
        std::cout << "SUCCESS: " << metricsPath << " (" << positions.size() << " slices, " << totalMs << " ms)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }
}
