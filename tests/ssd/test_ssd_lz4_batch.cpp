#include "erwt3d/reader.hpp"
#include "erwt3d/writer.hpp"
#include "erwt3d/ssd/ssd_config.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

bool testSsdBatchRuns() {
    const char* testPath = "/tmp/test_ssd_lz4_b.erwt3d";
    const char* rawPath = "/tmp/test_ssd_lz4_b_raw.raw";
    const uint64_t NX = 64, NY = 64, NZ = 64;

    {
        FILE* f = fopen(rawPath, "wb");
        if (!f) return false;
        std::vector<float> data(NX * NY * NZ);
        for (size_t i = 0; i < data.size(); ++i)
            data[i] = static_cast<float>(i) * 0.0001f;
        fwrite(data.data(), sizeof(float), data.size(), f);
        fclose(f);
    }

    erwt3d::writeERWT3DFromFile(testPath, rawPath, NX, NY, NZ, 64, 64, 64, 4, 4, 4, 1, 256);

    const size_t se = NY * NZ;
    std::vector<float> bufSsd(se * 2);
    std::vector<float> bufHdd(se * 2);

    {
        erwt3d::ERWT3DReader r(testPath);
        r.setSBReadMode(erwt3d::SBReadMode::SSDConcurrentExtent);
        erwt3d::SSDReadConfig sc;
        sc.read_threads = 1;
        sc.decode_threads = 1;
        sc.read_window_bytes = 4096;
        sc.max_gap_bytes = 0;
        r.setSSDReadConfig(sc);
        std::vector<erwt3d::ERWT3DReader::SliceBatchRequest> reqs;
        reqs.push_back({erwt3d::SliceAxis::X, 0, bufSsd.data()});
        reqs.push_back({erwt3d::SliceAxis::X, 1, bufSsd.data() + se});
        assert(r.readSlicesBatch(reqs, 1, 256, {}));
    }

    {
        erwt3d::ERWT3DReader r(testPath);
        r.setSBReadMode(erwt3d::SBReadMode::HDDReadWindow);
        r.setHDDReadWindowConfig({64ULL * 1024, 0});
        std::vector<erwt3d::ERWT3DReader::SliceBatchRequest> reqs;
        reqs.push_back({erwt3d::SliceAxis::X, 0, bufHdd.data()});
        reqs.push_back({erwt3d::SliceAxis::X, 1, bufHdd.data() + se});
        assert(r.readSlicesBatch(reqs, 1, 256, {}));
    }

    size_t mismatches = 0;
    for (size_t i = 0; i < se * 2; ++i) {
        if (bufSsd[i] != bufHdd[i]) ++mismatches;
    }
    assert(mismatches == 0);

    remove(testPath);
    remove(rawPath);
    return true;
}

} // anonymous namespace

int main() {
    if (!testSsdBatchRuns()) {
        printf("SSD LZ4 batch test: FAIL\n");
        return 1;
    }
    printf("SSD LZ4 batch test: PASS (SSD vs HDD byte-identical)\n");
    return 0;
}
