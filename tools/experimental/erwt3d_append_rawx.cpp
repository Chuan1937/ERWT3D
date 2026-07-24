#include "erwt3d/raw_x_aux.hpp"
#include "erwt3d/rzfp_format.hpp"
#include "erwt3d/rzfp_writer.hpp"

#include <cstdio>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char* argv[]) {
    if (argc < 6) {
        fprintf(stderr, "Usage: erwt3d_append_rawx <rzfp_file> <raw_file> <nx> <ny> <nz>\n");
        return 1;
    }
    std::string rzfp = argv[1];
    std::string raw = argv[2];
    uint64_t nx = std::stoull(argv[3]);
    uint64_t ny = std::stoull(argv[4]);
    uint64_t nz = std::stoull(argv[5]);

    erwt3d::RawXAuxStats stats;
    bool ok = erwt3d::appendRawXAuxToRzfpFile(rzfp, raw, nx, ny, nz, &stats, false);
    printf("Result: %s\n", ok ? "OK" : "FAILED");
    printf("Message: %s\n", stats.message.c_str());
    if (stats.stored()) {
        printf("Raw X aux bytes: %llu MB\n",
               (unsigned long long)(stats.raw_x_aux_bytes / (1024*1024)));
        printf("Total ratio: %.3fx\n", stats.total_storage_ratio);
    }
    return ok ? 0 : 1;
}
