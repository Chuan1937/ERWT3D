#include <erwt3d/raw_x_aux.hpp>
#include <erwt3d/taps_format.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

using namespace erwt3d;

static void printUsage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s --taps-dir data.taps --axis X|Y|Z --index N --output slice.raw\n"
        "       %s --taps-dir data.taps --info\n",
        prog, prog);
}

int main(int argc, char* argv[]) {
    std::string taps_dir;
    char axis = 0;
    uint64_t index = UINT64_MAX;
    std::string output_path;
    bool info_only = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--taps-dir" && i + 1 < argc) { taps_dir = argv[++i]; }
        else if (arg == "--axis" && i + 1 < argc) { axis = argv[++i][0]; }
        else if (arg == "--index" && i + 1 < argc) { index = std::stoull(argv[++i]); }
        else if (arg == "--output" && i + 1 < argc) { output_path = argv[++i]; }
        else if (arg == "--info") { info_only = true; }
        else { printUsage(argv[0]); return 1; }
    }

    if (taps_dir.empty()) { printUsage(argv[0]); return 1; }

    TapsReader reader(taps_dir);

    if (info_only) {
        std::fprintf(stdout, "TAPS format info:\n");
        std::fprintf(stdout, "  Dimensions: %lu x %lu x %lu\n", reader.nx(), reader.ny(), reader.nz());
        std::fprintf(stdout, "  Storage ratio: %.4fx\n", reader.storageRatio());
        return 0;
    }

    if (axis == 0 || index == UINT64_MAX || output_path.empty()) {
        printUsage(argv[0]);
        return 1;
    }

    uint64_t plane_floats = 0;
    switch (axis) {
        case 'X': plane_floats = reader.ny() * reader.nz(); break;
        case 'Y': plane_floats = reader.nx() * reader.nz(); break;
        case 'Z': plane_floats = reader.nx() * reader.ny(); break;
    }

    std::vector<float> buf(plane_floats);
    if (!reader.readSlice(axis, index, buf.data())) {
        std::fprintf(stderr, "Failed to read %c[%lu]\n", axis, index);
        return 1;
    }

    ScopedFd fd(open(output_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644));
    if (!fd.valid()) {
        std::fprintf(stderr, "Cannot create %s\n", output_path.c_str());
        return 1;
    }
    writeFullyAt(fd.get(), buf.data(), plane_floats * sizeof(float), 0);

    std::fprintf(stderr, "Wrote %c[%lu]: %lu floats to %s\n",
                 axis, index, plane_floats, output_path.c_str());
    return 0;
}
