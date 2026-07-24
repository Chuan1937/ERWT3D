#include "erwt3d/lz4_axis_plane_writer.hpp"
#include "erwt3d/rzfp_axis_plane_writer.hpp"
#include "erwt3d/axis_plane.hpp"
#include "erwt3d/file_format_detect.hpp"
#include "erwt3d/format.hpp"
#include "erwt3d/rzfp_format.hpp"

#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <chrono>
#include <unistd.h>

using namespace erwt3d;

static void printUsage() {
    std::cerr << "Usage: erwt3d_axis_plane_gen --input <erwt3d_or_rzfp>"
                 " --raw <raw.dat>"
                 " --axis <X|Y|Z>"
                 " [--chunk-elements <N>]"
                 " [--threads <N>]"
                 " [--storage-budget <ratio>]\n";
}

int main(int argc, char* argv[]) {
    std::string inputPath, rawPath, axisStr;
    uint32_t chunkElements = 128 * 1024;
    int threads = 8;
    double storageBudget = 100.0; // no limit by default

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--input" && i + 1 < argc) inputPath = argv[++i];
        else if (arg == "--raw" && i + 1 < argc) rawPath = argv[++i];
        else if (arg == "--axis" && i + 1 < argc) axisStr = argv[++i];
        else if (arg == "--chunk-elements" && i + 1 < argc)
            chunkElements = static_cast<uint32_t>(std::stoul(argv[++i]));
        else if (arg == "--threads" && i + 1 < argc)
            threads = std::stoi(argv[++i]);
        else if (arg == "--storage-budget" && i + 1 < argc)
            storageBudget = std::stod(argv[++i]);
        else {
            printUsage();
            return 1;
        }
    }

    if (inputPath.empty() || rawPath.empty() || axisStr.empty()) {
        printUsage();
        return 1;
    }

    PlaneAxis axis;
    if (axisStr == "X" || axisStr == "x") axis = PlaneAxis::X;
    else if (axisStr == "Y" || axisStr == "y") axis = PlaneAxis::Y;
    else if (axisStr == "Z" || axisStr == "z") axis = PlaneAxis::Z;
    else { printUsage(); return 1; }

    auto fmt = detectOptimizedFileFormat(inputPath);
    bool isLz4 = (fmt == OptimizedFileFormat::LZ4_ERWT3D);
    bool isRzfp = (fmt == OptimizedFileFormat::RZFP);

    if (!isLz4 && !isRzfp) {
        std::cerr << "Error: unrecognized file format\n";
        return 1;
    }

    std::cout << "Format: " << (isLz4 ? "LZ4" : "RZFP")
              << ", axis: " << axisLabel(axis) << "\n";

    uint64_t nx = 0, ny = 0, nz = 0;
    if (isLz4) {
        ERWT3DHeader hdr{};
        int fd = open(inputPath.c_str(), O_RDONLY);
        if (fd < 0 || read(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
            std::cerr << "Error: cannot read LZ4 header\n";
            return 1;
        }
        close(fd);
        nx = hdr.nx; ny = hdr.ny; nz = hdr.nz;
    } else {
        RzfpFileHeader hdr{};
        int fd = open(inputPath.c_str(), O_RDONLY);
        if (fd < 0 || read(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
            std::cerr << "Error: cannot read RZFP header\n";
            return 1;
        }
        close(fd);
        nx = hdr.nx; ny = hdr.ny; nz = hdr.nz;
    }

    std::cout << "Dims: " << nx << " x " << ny << " x " << nz << "\n";
    std::cout << "Chunk elements: " << chunkElements << ", threads: " << threads << "\n";

    auto t0 = std::chrono::high_resolution_clock::now();

    bool ok = false;
    if (isLz4) {
        Lz4AxisPlaneWriterStats stats;
        ok = writeLz4AxisPlaneSidecar(rawPath, inputPath, axis,
                                        nx, ny, nz, chunkElements,
                                        storageBudget, threads, &stats);
        if (ok) {
            std::cout << "Sidecar: " << axisPlaneSidecarPath(inputPath, axis) << "\n";
            std::cout << "  Sidecar bytes: " << stats.sidecar_bytes
                      << " (" << (stats.sidecar_bytes / 1048576.0) << " MB)\n";
            std::cout << "  Compression ratio: " << stats.compression_ratio << "x\n";
            std::cout << "  Planes: " << stats.plane_count << "\n";
        }
    } else {
        RzfpXPlaneCodecConfig codecCfg{};
        codecCfg.error.policy = RelativeErrorPolicy::Strict;
        codecCfg.error.contest_bound = 0.001;
        RzfpAxisPlaneWriterStats stats;
        ok = writeRzfpAxisPlaneSidecar(rawPath, inputPath, axis, codecCfg,
                                         nx, ny, nz, threads, &stats);
        if (ok) {
            std::cout << "Sidecar: " << axisPlaneSidecarPath(inputPath, axis) << "\n";
            std::cout << "  Sidecar bytes: " << stats.sidecar_bytes
                      << " (" << (stats.sidecar_bytes / 1048576.0) << " MB)\n";
            std::cout << "  Compressed/Raw: " << stats.total_compressed_bytes
                      << " / " << stats.total_raw_bytes
                      << " (" << stats.compression_ratio << "x)\n";
            std::cout << "  Planes: " << stats.plane_count << "\n";
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();
    std::cout << "Time: " << elapsed << " s"
              << (ok ? "" : " (FAILED)") << "\n";

    return ok ? 0 : 1;
}
