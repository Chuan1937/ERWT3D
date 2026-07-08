#include "erwt3d/tri_writer.hpp"
#include "erwt3d/tri_format.hpp"
#include <iostream>
#include <thread>
#include <string>
#include <cstring>
#include <cstdlib>

void printUsage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " --raw PATH --output PATH --nx N --ny N --nz N [options]\n"
        << "  --codec zfp|raw       Compression codec (default: zfp)\n"
        << "  --rate N              ZFP fixed rate in bits/value (default: 16)\n"
        << "  --rel-tol V           Relative error threshold (default: 1e-3)\n"
        << "  --zero-abs-tol V      Near-zero absolute threshold (default: 1e-6)\n"
        << "  --threads N           Thread count (default: hardware_concurrency)\n"
        << "  --memory-limit-mb N   Memory limit in MB (default: 4096)\n";
}

int main(int argc, char* argv[]) {
    std::string rawPath, outPath;
    uint64_t nx = 0, ny = 0, nz = 0;
    std::string codecStr = "zfp";
    uint32_t rate = 16;
    double relTol = 1e-3;
    double zeroAbsTol = 1e-6;
    int threads = 0;
    size_t memoryLimitMB = 4096;

    auto next = [&](int& i) -> std::string {
        if (i + 1 >= argc) return "";
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--raw") rawPath = next(i);
        else if (arg == "--output" || arg == "--out") outPath = next(i);
        else if (arg == "--nx") nx = std::stoull(next(i));
        else if (arg == "--ny") ny = std::stoull(next(i));
        else if (arg == "--nz") nz = std::stoull(next(i));
        else if (arg == "--codec") codecStr = next(i);
        else if (arg == "--rate") rate = std::stoul(next(i));
        else if (arg == "--rel-tol") relTol = std::stod(next(i));
        else if (arg == "--zero-abs-tol") zeroAbsTol = std::stod(next(i));
        else if (arg == "--threads") threads = std::stoi(next(i));
        else if (arg == "--memory-limit-mb") memoryLimitMB = std::stoull(next(i));
        else if (arg == "--help" || arg == "-h") { printUsage(argv[0]); return 0; }
        else { std::cerr << "Unknown arg: " << arg << "\n"; return 1; }
    }

    if (rawPath.empty() || outPath.empty() || nx == 0 || ny == 0 || nz == 0) {
        std::cerr << "Error: --raw, --output, --nx, --ny, --nz are required\n";
        return 1;
    }

    if (threads <= 0) threads = (int)std::thread::hardware_concurrency();
    if (threads <= 0) threads = 1;

    uint32_t codec = erwt3d::TRI_CODEC_ZFP_FIXED_RATE;
    if (codecStr == "raw") codec = erwt3d::TRI_CODEC_RAW;

    bool ok = erwt3d::writeTriAxisLayout(
        rawPath, outPath, nx, ny, nz, codec, rate,
        relTol, zeroAbsTol, threads, memoryLimitMB);

    return ok ? 0 : 1;
}
