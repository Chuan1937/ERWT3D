#include "erwt3d/rzfp_auto_plan.hpp"
#include "erwt3d/rzfp_writer.hpp"
#include "erwt3d/rzfp_xplane_writer.hpp"
#include "erwt3d/raw_x_aux.hpp"

#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <string>
#include <unistd.h>

static void printUsage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " [options]\n\n"
        << "Options:\n"
        << "  --input PATH            Raw float32 file (required)\n"
        << "  --output PATH           RZFP output file (required)\n"
        << "  --nx N                  X dimension (required)\n"
        << "  --ny N                  Y dimension (required)\n"
        << "  --nz N                  Z dimension (required)\n"
        << "  --threads N             Encoder threads (default: 8)\n"
        << "  --memory-limit-mb N     Unused compatibility (default: 4096)\n"
        << "  --internal-rel-bound V  Internal relative bound (default: 0.00075)\n"
        << "  --contest-rel-bound V   Contest relative bound (default: 0.001)\n"
        << "  --exception-counts LIST e.g. 0,1,2,4,8,16\n"
        << "  --precisions LIST       e.g. 12,14,16,18,20,22,24\n"
        << "  --fill-modes LIST       zero,mean\n"
        << "  --physical-order zyx|v05-yzx (default: zyx)\n"
<< "  --xplane-sidecar        Generate 2D RZFP X-plane sidecar (.xp)\n"
         << "  --auto                  Auto-plan sidecar and read window\n"
         << "  --auto-time-limit N     Hard time limit for auto plan (default: 600)\n"
         << "  --auto-soft-time-limit N Soft time limit for auto plan (default: 300)\n"
         << "  --raw-x-aux MODE        Append raw X auxiliary region (auto|on|off, default: off)\n"
         << "  --force-storage-edge    Allow storage ratio up to 1.45x\n";
}

static std::vector<uint8_t> parseExceptionCounts(const std::string& s) {
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

static std::vector<erwt3d::RzfpExceptionFill> parseFillModes(const std::string& s) {
    std::vector<erwt3d::RzfpExceptionFill> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find(',', start);
        if (end == std::string::npos) end = s.size();
        std::string tok = s.substr(start, end - start);
        if (tok == "zero") out.push_back(erwt3d::RzfpExceptionFill::Zero);
        else if (tok == "mean") out.push_back(erwt3d::RzfpExceptionFill::Mean);
        else {
            std::cerr << "Unknown fill mode: " << tok << std::endl;
            std::exit(1);
        }
        start = end + 1;
    }
    return out;
}

int main(int argc, char* argv[]) {
    erwt3d::RzfpWriterConfig cfg;
    cfg.codec.error.policy = erwt3d::RelativeErrorPolicy::Strict;
    cfg.codec.error.contest_bound = 1e-3;
    cfg.codec.error.internal_bound = 7.5e-4;

    std::string inputPath;
    std::string outputPath;
    bool xplane_sidecar = false;
    bool auto_plan = false;
    uint64_t auto_time_limit = 600;
    uint64_t auto_soft_time_limit = 300;
    erwt3d::RawXAuxMode rawXAuxMode = erwt3d::RawXAuxMode::Off;
    bool forceStorageEdge = false;

    for (int i = 1; i < argc; ++i) {
        auto next = [&]() -> const char* {
            if (i + 1 < argc) return argv[++i];
            std::cerr << "Error: " << argv[i] << " requires a value\n";
            std::exit(1);
            return nullptr;
        };
        if (std::strcmp(argv[i], "--input") == 0) inputPath = next();
        else if (std::strcmp(argv[i], "--output") == 0) outputPath = next();
        else if (std::strcmp(argv[i], "--nx") == 0) cfg.nx = std::stoull(next());
        else if (std::strcmp(argv[i], "--ny") == 0) cfg.ny = std::stoull(next());
        else if (std::strcmp(argv[i], "--nz") == 0) cfg.nz = std::stoull(next());
        else if (std::strcmp(argv[i], "--threads") == 0) cfg.threads = std::stoi(next());
        else if (std::strcmp(argv[i], "--memory-limit-mb") == 0) cfg.memory_limit_mb = std::stoul(next());
        else if (std::strcmp(argv[i], "--internal-rel-bound") == 0) cfg.codec.error.internal_bound = std::stod(next());
        else if (std::strcmp(argv[i], "--contest-rel-bound") == 0) cfg.codec.error.contest_bound = std::stod(next());
        else if (std::strcmp(argv[i], "--exception-counts") == 0) cfg.codec.optional_exception_counts = parseExceptionCounts(next());
        else if (std::strcmp(argv[i], "--precisions") == 0) cfg.codec.precisions = parsePrecisions(next());
        else if (std::strcmp(argv[i], "--fill-modes") == 0) cfg.codec.fill_modes = parseFillModes(next());
        else if (std::strcmp(argv[i], "--physical-order") == 0) {
            std::string v = next();
            if (v == "v05-yzx" || v == "V05_YZX") cfg.physical_order = erwt3d::PhysicalOrder::V05_YZX;
            else if (v == "zyx" || v == "ZYX") cfg.physical_order = erwt3d::PhysicalOrder::ZYX;
            else {
                std::cerr << "Unknown physical order: " << v << std::endl;
                return 1;
            }
        } else if (std::strcmp(argv[i], "--xplane-sidecar") == 0) {
            xplane_sidecar = true;
        } else if (std::strcmp(argv[i], "--auto") == 0) {
            auto_plan = true;
        } else if (std::strcmp(argv[i], "--auto-time-limit") == 0) {
            auto_time_limit = std::stoull(next());
        } else if (std::strcmp(argv[i], "--auto-soft-time-limit") == 0) {
            auto_soft_time_limit = std::stoull(next());
        } else if (std::strcmp(argv[i], "--raw-x-aux") == 0) {
            std::string mode = next();
            if (mode == "on") rawXAuxMode = erwt3d::RawXAuxMode::On;
            else if (mode == "auto") rawXAuxMode = erwt3d::RawXAuxMode::Auto;
            else if (mode == "off") rawXAuxMode = erwt3d::RawXAuxMode::Off;
            else {
                std::cerr << "Error: unknown --raw-x-aux mode: " << mode << " (valid: auto, on, off)" << std::endl;
                return 1;
            }
        } else if (std::strcmp(argv[i], "--force-storage-edge") == 0) {
            forceStorageEdge = true;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << argv[i] << std::endl;
            printUsage(argv[0]);
            return 1;
        }
    }

    if (inputPath.empty() || outputPath.empty() || cfg.nx == 0 || cfg.ny == 0 || cfg.nz == 0) {
        std::cerr << "Error: --input, --output, --nx, --ny, --nz are required\n";
        printUsage(argv[0]);
        return 1;
    }

    if (auto_plan) {
        int fd = open(inputPath.c_str(), O_RDONLY);
        if (fd < 0) {
            std::cerr << "Error: cannot open raw file for auto plan: " << inputPath << std::endl;
            return 1;
        }
        erwt3d::RzfpAutoPlanConfig plan_cfg;
        plan_cfg.time_limit_seconds = auto_time_limit;
        plan_cfg.soft_time_limit_seconds = auto_soft_time_limit;
        plan_cfg.main_codec_config = cfg.codec;
        plan_cfg.sidecar_codec_config.error = cfg.codec.error;
        plan_cfg.sidecar_codec_config.precisions = cfg.codec.precisions;

        erwt3d::RzfpAutoPlanResult result;
        if (!erwt3d::runRzfpAutoPlan(fd, cfg.nx, cfg.ny, cfg.nz, plan_cfg, result)) {
            close(fd);
            std::cerr << "Error: auto plan failed" << std::endl;
            return 1;
        }
        close(fd);

        std::cout << result.toJson() << std::endl;
        xplane_sidecar = result.enable_x_sidecar;
    }

    erwt3d::RzfpWriterStats stats{};
    bool ok = erwt3d::writeRzfpFile(inputPath, outputPath, cfg, &stats);
    if (!ok) return 1;

    std::cout << "RZFP conversion complete: " << outputPath << std::endl;
    std::cout << "storage_ratio: " << stats.storage_ratio << std::endl;

if (rawXAuxMode != erwt3d::RawXAuxMode::Off) {
        erwt3d::RawXAuxStats auxStats;
        bool auxOk = erwt3d::appendRawXAuxToRzfpFile(outputPath, inputPath, cfg.nx, cfg.ny, cfg.nz,
                                                       &auxStats, forceStorageEdge);
        if (!auxOk && rawXAuxMode == erwt3d::RawXAuxMode::On) {
            std::cerr << "Error: Raw X auxiliary required but generation failed" << std::endl;
            return 1;
        }
        if (auxStats.stored()) {
            std::cout << "Raw X auxiliary: stored"
                      << " (" << (auxStats.raw_x_aux_bytes / (1024*1024)) << " MB)"
                      << ", total ratio: " << std::fixed << std::setprecision(3)
                      << auxStats.total_storage_ratio << "x" << std::endl;
        } else {
            std::cout << "Raw X auxiliary: skipped"
                      << " (" << auxStats.message << ")" << std::endl;
        }
    }

    if (xplane_sidecar) {
        erwt3d::RzfpXPlaneCodecConfig xcfg;
        xcfg.error = cfg.codec.error;
        xcfg.precisions = cfg.codec.precisions;

        const std::string sidecar_path = outputPath + ".xp";
        erwt3d::RzfpXPlaneWriterStats xstats{};
        bool xok = erwt3d::writeXPlaneSidecarFile(
            inputPath, sidecar_path, xcfg, cfg.nx, cfg.ny, cfg.nz, cfg.threads, &xstats);
        if (!xok) return 1;

        std::cout << "X-plane sidecar: " << sidecar_path << std::endl;
        std::cout << "sidecar_ratio: " << std::fixed << std::setprecision(4) << xstats.compression_ratio << std::endl;
        std::cout << "combined_ratio: " << std::fixed << std::setprecision(4)
                  << (stats.storage_ratio + xstats.compression_ratio) << std::endl;
    }

    return 0;
}
