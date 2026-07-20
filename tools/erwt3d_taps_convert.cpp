#include <erwt3d/taps_format.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace erwt3d;

int main(int argc, char* argv[]) {
    std::string input_path;
    std::string output_dir;
    uint64_t nx = 0, ny = 0, nz = 0;
    std::string codec_str = "lz4";
    uint64_t chunk_kb = 1024;
    int threads = 8;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--input" && i + 1 < argc) { input_path = argv[++i]; }
        else if (arg == "--output-dir" && i + 1 < argc) { output_dir = argv[++i]; }
        else if (arg == "--nx" && i + 1 < argc) { nx = std::stoull(argv[++i]); }
        else if (arg == "--ny" && i + 1 < argc) { ny = std::stoull(argv[++i]); }
        else if (arg == "--nz" && i + 1 < argc) { nz = std::stoull(argv[++i]); }
        else if (arg == "--codec" && i + 1 < argc) { codec_str = argv[++i]; }
        else if (arg == "--chunk-kb" && i + 1 < argc) { chunk_kb = std::stoull(argv[++i]); }
        else if (arg == "--threads" && i + 1 < argc) { threads = std::stoi(argv[++i]); }
        else {
            std::fprintf(stderr,
                "Usage: %s --input data.raw --output-dir data.taps "
                "--nx N --ny N --nz N [--codec lz4|rzfp2d] "
                "[--chunk-kb N] [--threads N]\n", argv[0]);
            return 1;
        }
    }

    if (input_path.empty() || output_dir.empty() || nx == 0 || ny == 0 || nz == 0) {
        std::fprintf(stderr, "Missing required arguments\n");
        return 1;
    }

    TapsWriteConfig config;
    config.nx = nx; config.ny = ny; config.nz = nz;
    config.chunk_kb = chunk_kb;
    config.threads = threads;
    config.output_dir = output_dir;

    if (codec_str == "rzfp2d") {
        config.codec = TapsCodec::RZFP2D;
    } else {
        config.codec = TapsCodec::LZ4;
    }

    TapsStats stats;
    if (!tapsWriteFromRaw(input_path, config, stats)) {
        std::fprintf(stderr, "TAPS write failed\n");
        return 1;
    }

    return 0;
}
