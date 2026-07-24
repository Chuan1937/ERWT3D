#include "erwt3d/rzfp_axis_leaf.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void usage() {
    std::cerr
        << "Usage: erwt3d_rzfp_axis_repack"
        << " --input <source.rzfp>"
        << " --output <axis.rzfp>"
        << " [--memory-limit-mb <N>]\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string input;
    std::string output;
    size_t memoryLimitMiB = 1024;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--input" && i + 1 < argc) {
            input = argv[++i];
        } else if (arg == "--output" &&
                   i + 1 < argc) {
            output = argv[++i];
        } else if (arg == "--memory-limit-mb" &&
                   i + 1 < argc) {
            memoryLimitMiB = static_cast<size_t>(
                std::stoull(argv[++i]));
        } else {
            usage();
            return EXIT_FAILURE;
        }
    }

    if (input.empty() || output.empty() ||
        memoryLimitMiB < 256) {
        usage();
        return EXIT_FAILURE;
    }

    erwt3d::RzfpAxisLeafRepackStats stats;
    if (!erwt3d::repackRzfpAxisLeaves(
            input,
            output,
            memoryLimitMiB,
            &stats)) {
        return EXIT_FAILURE;
    }

    std::cout
        << "metadata_bytes=" << stats.metadata_bytes
        << "\nx_replica_bytes=" << stats.replica_bytes[0]
        << "\ny_replica_bytes=" << stats.replica_bytes[1]
        << "\nz_replica_bytes=" << stats.replica_bytes[2]
        << "\ntotal_bytes=" << stats.total_bytes
        << "\nstorage_ratio=" << stats.storage_ratio
        << "\n";
    return stats.storage_ratio <= 1.5
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}
