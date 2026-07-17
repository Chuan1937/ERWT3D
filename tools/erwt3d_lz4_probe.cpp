#include "erwt3d/lz4_probe.hpp"
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>

int main(int argc, char** argv) {
    std::string raw_path;
    erwt3d::Lz4ProbeConfig cfg;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 < argc) return argv[++i];
            std::cerr << "Error: " << arg << " requires a value" << std::endl;
            std::exit(1);
            return nullptr;
        };
        if (arg == "--input" || arg == "-i") raw_path = next();
        else if (arg == "--nx") cfg.nx = std::stoull(next());
        else if (arg == "--ny") cfg.ny = std::stoull(next());
        else if (arg == "--nz") cfg.nz = std::stoull(next());
        else if (arg == "--threads") cfg.threads = std::stoi(next());
        else if (arg == "--slabs") cfg.slabs_to_sample = std::stoul(next());
        else if (arg == "--sb-per-slab") cfg.superblocks_per_slab = std::stoul(next());
        else if (arg == "--skip-threshold") cfg.skip_threshold = std::stod(next());
        else if (arg == "--json") { /* handled after probe */ }
    }

    if (raw_path.empty() || cfg.nx == 0 || cfg.ny == 0 || cfg.nz == 0) {
        std::cerr << "Usage: erwt3d_lz4_probe --input PATH --nx N --ny N --nz N [options]\n"
                  << "  --threads N       (default: 1)\n"
                  << "  --slabs N         (default: 4)\n"
                  << "  --sb-per-slab N   (default: 32)\n"
                  << "  --skip-threshold V (default: 0.90)\n"
                  << "  --json            Output JSON format\n";
        return 1;
    }

    auto result = erwt3d::probeLz4Compression(raw_path, cfg);

    std::cout << std::fixed << std::setprecision(4);
    if (result.skipped) {
        std::cout << "Probe skipped: " << result.skip_reason << std::endl;
        return 0;
    }

    std::cout << "LZ4 Compression Probe Results:" << std::endl;
    std::cout << "  main_ratio_estimate: " << result.main_ratio_estimate << std::endl;
    std::cout << "  main_ratio_lower:   " << result.main_ratio_lower << std::endl;
    std::cout << "  main_ratio_upper:   " << result.main_ratio_upper << std::endl;
    std::cout << "  compressed_fraction: " << result.compressed_block_fraction
              << " (" << (result.compressed_block_fraction * 100) << "%)" << std::endl;
    std::cout << "  sampled_superblocks: " << result.sampled_superblocks << std::endl;
    std::cout << "  sampled_raw_bytes:   " << result.sampled_raw_bytes << std::endl;
    std::cout << "  elapsed_seconds:     " << result.elapsed_seconds << std::endl;

    if (result.main_ratio_estimate > cfg.skip_threshold) {
        std::cout << "  COMPRESSION NOT RECOMMENDED (ratio " << result.main_ratio_estimate
                  << " > " << cfg.skip_threshold << ")" << std::endl;
    } else {
        std::cout << "  COMPRESSION RECOMMENDED (ratio " << result.main_ratio_estimate
                  << " <= " << cfg.skip_threshold << ")" << std::endl;
    }

    return 0;
}
