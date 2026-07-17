#include "erwt3d/auto_plan.hpp"
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <fstream>

int main(int argc, char** argv) {
    std::string raw_path, output_json;
    uint64_t nx = 0, ny = 0, nz = 0;
    int threads = 8;
    double storage_budget = 1.45;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 < argc) return argv[++i];
            std::cerr << "Error: " << arg << " requires a value\n";
            std::exit(1);
            return nullptr;
        };
        if (arg == "--raw") raw_path = next();
        else if (arg == "--nx") nx = std::stoull(next());
        else if (arg == "--ny") ny = std::stoull(next());
        else if (arg == "--nz") nz = std::stoull(next());
        else if (arg == "--threads") threads = std::stoi(next());
        else if (arg == "--storage-budget") storage_budget = std::stod(next());
        else if (arg == "--output") output_json = next();
    }

    if (raw_path.empty() || nx == 0 || ny == 0 || nz == 0) {
        std::cerr << "Usage: erwt3d_auto_plan --raw PATH --nx N --ny N --nz N [options]\n"
                  << "  --threads N         (default: 8)\n"
                  << "  --storage-budget X  (default: 1.45)\n"
                  << "  --output FILE.json  (optional)\n";
        return 1;
    }

    auto result = erwt3d::planFormat(raw_path, nx, ny, nz, threads, storage_budget);

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "\n========== ERWT3D Auto Plan ==========\n";
    std::cout << "Disk: " << result.disk_cfg.sequential_mb_s << " MB/s, "
              << result.disk_cfg.seek_ms << " ms seek\n";
    std::cout << "LZ4 probe: ratio=" << result.lz4_probe.main_ratio_estimate
              << " [" << result.lz4_probe.main_ratio_lower << "-"
              << result.lz4_probe.main_ratio_upper << "]\n";
    std::cout << "RZFP available: " << (result.rzfp_available ? "yes" : "no") << "\n\n";

    std::cout << "Candidates:\n";
    for (const auto& c : result.alternatives) {
        std::cout << "  " << c.name << ": T=" << c.predicted_t_composite
                  << "s ratio=" << c.total_ratio_mean
                  << " feasible=" << (c.feasible ? "yes" : "no")
                  << " conf=" << c.confidence
                  << " " << c.reason << "\n";
    }

    std::cout << "\nRecommended: " << result.recommended.name << "\n";
    std::cout << "  uncertainty: " << (result.recommended.uncertain ? "yes" : "no") << "\n";
    std::cout << "  reason: " << result.recommended.reason << "\n";
    std::cout << "  elapsed: " << result.elapsed_seconds << "s\n";

    if (!output_json.empty()) {
        std::ofstream f(output_json);
        f << "{\n";
        f << "  \"recommended\": \"" << result.recommended.name << "\",\n";
        f << "  \"total_ratio\": " << result.recommended.total_ratio_mean << ",\n";
        f << "  \"predicted_t_composite\": " << result.recommended.predicted_t_composite << ",\n";
        f << "  \"uncertain\": " << (result.recommended.uncertain ? "true" : "false") << ",\n";
        f << "  \"disk_sequential_mb_s\": " << result.disk_cfg.sequential_mb_s << ",\n";
        f << "  \"disk_seek_ms\": " << result.disk_cfg.seek_ms << "\n";
        f << "}\n";
        std::cout << "Saved: " << output_json << "\n";
    }

    return 0;
}
