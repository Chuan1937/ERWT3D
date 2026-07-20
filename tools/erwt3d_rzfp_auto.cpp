#include "erwt3d/rzfp_auto_plan.hpp"

#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <string>
#include "erwt3d/platform_io.hpp"

namespace {

struct Options {
    std::string raw_path;
    std::string output_plan;
    uint64_t nx = 0;
    uint64_t ny = 0;
    uint64_t nz = 0;
    uint64_t time_limit = 600;
    uint64_t soft_time_limit = 300;
    uint32_t seed = 20260511;
    double rel_bound = 1e-3;
    bool no_sidecar = false;
};

static void printUsage(const char* prog) {
    std::cerr << "Usage: " << prog << " --raw PATH --nx N --ny N --nz N [options]\n"
              << "Options:\n"
              << "  --output-plan PATH     Write plan JSON to PATH\n"
              << "  --time-limit N         Hard time limit in seconds (default: 600)\n"
              << "  --soft-time-limit N    Soft time limit in seconds (default: 300)\n"
              << "  --seed N               Random seed (default: 20260511)\n"
              << "  --rel-bound V          Contest relative bound (default: 0.001)\n"
              << "  --no-sidecar           Do not evaluate X-plane sidecar\n";
}

static Options parseOptions(int argc, char* argv[]) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        auto next = [&]() -> const char* {
            if (i + 1 < argc) return argv[++i];
            std::cerr << "Error: " << argv[i] << " requires a value\n";
            std::exit(1);
            return nullptr;
        };
        if (std::strcmp(argv[i], "--raw") == 0) opt.raw_path = next();
        else if (std::strcmp(argv[i], "--nx") == 0) opt.nx = std::stoull(next());
        else if (std::strcmp(argv[i], "--ny") == 0) opt.ny = std::stoull(next());
        else if (std::strcmp(argv[i], "--nz") == 0) opt.nz = std::stoull(next());
        else if (std::strcmp(argv[i], "--output-plan") == 0) opt.output_plan = next();
        else if (std::strcmp(argv[i], "--time-limit") == 0) opt.time_limit = std::stoull(next());
        else if (std::strcmp(argv[i], "--soft-time-limit") == 0) opt.soft_time_limit = std::stoull(next());
        else if (std::strcmp(argv[i], "--seed") == 0) opt.seed = static_cast<uint32_t>(std::stoul(next()));
        else if (std::strcmp(argv[i], "--rel-bound") == 0) opt.rel_bound = std::stod(next());
        else if (std::strcmp(argv[i], "--no-sidecar") == 0) opt.no_sidecar = true;
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]); std::exit(0);
        } else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            printUsage(argv[0]); std::exit(1);
        }
    }
    return opt;
}

} // namespace

int main(int argc, char* argv[]) {
    Options opt = parseOptions(argc, argv);
    if (opt.raw_path.empty() || opt.nx == 0 || opt.ny == 0 || opt.nz == 0) {
        std::cerr << "Error: --raw, --nx, --ny, --nz are required\n";
        printUsage(argv[0]);
        return 1;
    }

    int fd = io_open(opt.raw_path.c_str(), O_RDONLY);
    if (fd < 0) {
        std::cerr << "Error: cannot open raw file: " << opt.raw_path << std::endl;
        return 1;
    }

    erwt3d::RzfpAutoPlanConfig cfg;
    cfg.time_limit_seconds = opt.time_limit;
    cfg.soft_time_limit_seconds = opt.soft_time_limit;
    cfg.random_seed = opt.seed;
    cfg.evaluate_x_sidecar = !opt.no_sidecar;
    cfg.main_codec_config.error.contest_bound = opt.rel_bound;
    cfg.main_codec_config.error.internal_bound = opt.rel_bound * 0.75;
    cfg.main_codec_config.error.policy = erwt3d::RelativeErrorPolicy::Strict;
    cfg.sidecar_codec_config.error.contest_bound = opt.rel_bound;
    cfg.sidecar_codec_config.error.internal_bound = opt.rel_bound * 0.75;
    cfg.sidecar_codec_config.error.policy = erwt3d::RelativeErrorPolicy::Strict;

    erwt3d::RzfpAutoPlanResult result;
    if (!erwt3d::runRzfpAutoPlan(fd, opt.nx, opt.ny, opt.nz, cfg, result)) {
        std::cerr << "Error: auto plan failed" << std::endl;
        io_close(fd);
        return 1;
    }
    io_close(fd);

    const std::string json = result.toJson();
    if (!opt.output_plan.empty()) {
        std::ofstream f(opt.output_plan);
        if (!f) {
            std::cerr << "Error: cannot write plan file: " << opt.output_plan << std::endl;
            return 1;
        }
        f << json;
    }
    std::cout << json << std::endl;

    return 0;
}
