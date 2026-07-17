#include "erwt3d/rzfp_xplane_writer.hpp"

#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

static std::vector<uint8_t> parsePrecisions(const std::string& s) {
    std::vector<uint8_t> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find(',', start);
        if (end == std::string::npos) end = s.size();
        int v = std::stoi(s.substr(start, end - start));
        out.push_back(static_cast<uint8_t>(v));
        start = end + 1;
    }
    return out;
}

static void printUsage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " [options]\n\n"
        << "Options:\n"
        << "  --raw PATH              Raw float32 file (required)\n"
        << "  --output PATH           X-plane sidecar output file (required)\n"
        << "  --nx N                  X dimension (required)\n"
        << "  --ny N                  Y dimension (required)\n"
        << "  --nz N                  Z dimension (required)\n"
        << "  --threads N             Encoder threads (default: 8)\n"
        << "  --fast                  Use fast accuracy-only encoding\n"
        << "  --precisions LIST       Precision candidates, e.g. 16,18,20,22 (default full)\n"
        << "  --contest-rel-bound V   Contest relative bound (default: 0.001)\n"
        << "  --internal-rel-bound V  Internal relative bound (default: 0.00075)\n";
}

int main(int argc, char* argv[]) {
    std::string raw_path;
    std::string output_path;
    uint64_t nx = 0;
    uint64_t ny = 0;
    uint64_t nz = 0;
    int threads = 8;
    bool fast = false;
    std::vector<uint8_t> custom_precisions;
    double contest_bound = 1e-3;
    double internal_bound = 7.5e-4;

    for (int i = 1; i < argc; ++i) {
        auto next = [&]() -> const char* {
            if (i + 1 < argc) return argv[++i];
            std::cerr << "Error: " << argv[i] << " requires a value\n";
            std::exit(1);
            return nullptr;
        };
        if (std::strcmp(argv[i], "--raw") == 0) raw_path = next();
        else if (std::strcmp(argv[i], "--output") == 0) output_path = next();
        else if (std::strcmp(argv[i], "--nx") == 0) nx = std::stoull(next());
        else if (std::strcmp(argv[i], "--ny") == 0) ny = std::stoull(next());
        else if (std::strcmp(argv[i], "--nz") == 0) nz = std::stoull(next());
        else if (std::strcmp(argv[i], "--threads") == 0) threads = std::stoi(next());
        else if (std::strcmp(argv[i], "--fast") == 0) fast = true;
        else if (std::strcmp(argv[i], "--precisions") == 0) custom_precisions = parsePrecisions(next());
        else if (std::strcmp(argv[i], "--contest-rel-bound") == 0) contest_bound = std::stod(next());
        else if (std::strcmp(argv[i], "--internal-rel-bound") == 0) internal_bound = std::stod(next());
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << argv[i] << std::endl;
            printUsage(argv[0]);
            return 1;
        }
    }

    if (raw_path.empty() || output_path.empty() || nx == 0 || ny == 0 || nz == 0) {
        std::cerr << "Error: --raw, --output, --nx, --ny, --nz are required\n";
        printUsage(argv[0]);
        return 1;
    }

    erwt3d::RzfpXPlaneCodecConfig cfg;
    cfg.error.policy = erwt3d::RelativeErrorPolicy::Strict;
    cfg.error.contest_bound = contest_bound;
    cfg.error.internal_bound = internal_bound;
    cfg.fast_accuracy_only = fast;
    if (!custom_precisions.empty()) {
        cfg.precisions = custom_precisions;
        cfg.try_precision_exceptions = false;
    }

    erwt3d::RzfpXPlaneWriterStats stats{};
    bool ok = erwt3d::writeXPlaneSidecarFile(
        raw_path, output_path, cfg, nx, ny, nz, threads, &stats);
    if (!ok) return 1;

    std::cout << "X-plane sidecar complete: " << output_path << std::endl;
    std::cout << "sidecar_ratio: " << std::fixed << std::setprecision(4)
              << stats.compression_ratio << std::endl;
    return 0;
}
