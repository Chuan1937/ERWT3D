#include "erwt3d/writer.hpp"
#include "erwt3d/reader.hpp"
#include <iostream>
#include <vector>
#include <cmath>
#include <cstring>

using namespace erwt3d;

static int gFail = 0;

static float ref(uint64_t x, uint64_t y, uint64_t z) {
    return static_cast<float>(x + 1000*y + 1000000*z);
}

static void testPanelEquiv(uint64_t nx, uint64_t ny, uint64_t nz,
                            uint32_t stride, const std::vector<int>& threadCounts,
                            const char* label) {
    uint64_t n = nx * ny * nz;
    std::vector<float> orig(n);
    for (uint64_t z = 0; z < nz; ++z)
        for (uint64_t y = 0; y < ny; ++y)
            for (uint64_t x = 0; x < nx; ++x)
                orig[(z*ny + y)*nx + x] = ref(x, y, z);

    std::string path = std::string("/tmp/panel_") + label + ".erwt3d";
    if (!writeERWT3D(path, orig.data(), nx, ny, nz, 64,64,64, 4,4,4, 1,2048, 0, stride)) {
        std::cerr << label << " panel write fail" << std::endl;
        ++gFail; return;
    }

    // Verify header
    {
        ERWT3DReader r(path);
        const auto& h = r.getHeader();
        if (!hasXPanels(h)) {
            std::cerr << label << " X-panels NOT detected" << std::endl;
            ++gFail; goto cleanup;
        }
        if (hasYPanels(h) || hasZPanels(h)) {
            std::cerr << label << " unexpected Y/Z panels" << std::endl;
            ++gFail; goto cleanup;
        }
    }

    // Test 1: X-slices (hit + miss) with serial path
    for (auto axis : {SliceAxis::X, SliceAxis::Y, SliceAxis::Z}) {
        uint64_t limit = (axis==SliceAxis::X)?nx:(axis==SliceAxis::Y)?ny:nz;
        for (uint64_t idx : {uint64_t(0), limit/2, limit>0?limit-1:uint64_t(0)}) {
            uint64_t sz = (axis==SliceAxis::X)?ny*nz:(axis==SliceAxis::Y)?nx*nz:nx*ny;
            std::vector<float> out(sz);
            ERWT3DReader r(path);
            r.setIOBackend(IOBackend::Superblock);
            r.setSBParallelMode(SBParallelMode::Serial);
            if (!r.readSlice(axis, idx, out.data(), 1, 2048)) {
                std::cerr << label << " serial read fail axis=" << int(axis) << " idx=" << idx << std::endl;
                ++gFail; goto cleanup;
            }
            // Verify against reference
            for (uint64_t i = 0; i < sz; ++i) {
                uint64_t x, y, z;
                if (axis == SliceAxis::X) { x=idx; y=i%ny; z=i/ny; }
                else if (axis == SliceAxis::Y) { x=i%nx; y=idx; z=i/nx; }
                else { x=i%nx; y=(i/nx)%ny; z=idx; }
                float expected = ref(x, y, z);
                if (std::abs(out[i] - expected) > 1e-6f) {
                    std::cerr << label << " serial MISMATCH axis=" << int(axis)
                              << " idx=" << idx << " i=" << i
                              << " got=" << out[i] << " exp=" << expected << std::endl;
                    ++gFail; goto cleanup;
                }
            }
        }
    }

    // Test 2: parallel-read X-panel equivalence
    for (int nth : threadCounts) {
        // X-slice at hit index
        uint64_t hitX = 0; // local_x=0, always hit
        uint64_t missX = std::min(static_cast<uint64_t>(1), nx>1?uint64_t(1):uint64_t(0));
        for (uint64_t idx : {hitX, missX}) {
            if (idx >= nx) continue;
            bool expectHit = ((idx%64) % stride == 0);
            uint64_t sz = ny * nz;
            std::vector<float> sOut(sz), pOut(sz);

            ERWT3DReader rs(path);
            rs.setIOBackend(IOBackend::Superblock);
            rs.setSBParallelMode(SBParallelMode::Serial);
            if (!rs.readSlice(SliceAxis::X, idx, sOut.data(), 1, 2048)) {
                std::cerr << label << " t=" << nth << " serial fail x=" << idx << std::endl;
                ++gFail; goto cleanup;
            }

            ERWT3DReader rp(path);
            rp.setIOBackend(IOBackend::Superblock);
            rp.setSBParallelMode(SBParallelMode::ParallelRead);
            if (!rp.readSlice(SliceAxis::X, idx, pOut.data(), nth, 2048)) {
                std::cerr << label << " t=" << nth << " parallel fail x=" << idx << std::endl;
                ++gFail; goto cleanup;
            }

            for (uint64_t i = 0; i < sz; ++i) {
                if (std::abs(sOut[i] - pOut[i]) > 1e-6f) {
                    std::cerr << label << " t=" << nth << " serial/parallel MISMATCH x=" << idx
                              << " i=" << i << " s=" << sOut[i] << " p=" << pOut[i] << std::endl;
                    ++gFail; goto cleanup;
                }
            }
            if (expectHit) {
                std::cout << "  " << label << " x=" << idx << " t=" << nth << " (HIT): OK" << std::endl;
            } else {
                std::cout << "  " << label << " x=" << idx << " t=" << nth << " (miss/fallback): OK" << std::endl;
            }
        }
    }

    // Test 3: Y/Z slices on panel file match non-panel output
    {
        std::string noPanelPath = std::string("/tmp/panel_") + label + "_np.erwt3d";
        writeERWT3D(noPanelPath, orig.data(), nx, ny, nz, 64,64,64, 4,4,4, 1,2048, 0, 0);

        for (auto axis : {SliceAxis::Y, SliceAxis::Z}) {
            uint64_t limit = (axis==SliceAxis::Y)?ny:nz;
            uint64_t idx = limit/2;
            uint64_t sz = (axis==SliceAxis::Y)?nx*nz:nx*ny;

            ERWT3DReader r1(path); // panel file
            r1.setIOBackend(IOBackend::Superblock);
            r1.setSBParallelMode(SBParallelMode::Serial);
            std::vector<float> o1(sz);
            r1.readSlice(axis, idx, o1.data(), 1, 2048);

            ERWT3DReader r2(noPanelPath); // non-panel file
            r2.setIOBackend(IOBackend::Superblock);
            r2.setSBParallelMode(SBParallelMode::Serial);
            std::vector<float> o2(sz);
            r2.readSlice(axis, idx, o2.data(), 1, 2048);

            for (uint64_t i = 0; i < sz; ++i) {
                if (std::abs(o1[i] - o2[i]) > 1e-6f) {
                    std::cerr << label << " panel vs no-panel MISMATCH axis=" << int(axis)
                              << " idx=" << idx << " i=" << i << std::endl;
                    ++gFail;
                    std::remove(noPanelPath.c_str());
                    goto cleanup;
                }
            }
        }
        std::remove(noPanelPath.c_str());
        std::cout << "  " << label << " Y/Z no-regression: OK" << std::endl;
    }

cleanup:
    std::remove(path.c_str());
}

int main() {
    std::cout << "Panel Index Tests" << std::endl;
    std::cout << "=================" << std::endl;

    std::vector<int> threads = {2, 4, 8};

    testPanelEquiv(100, 100, 100, 4, threads, "100^3_s4");
    testPanelEquiv(65, 66, 67, 4, threads, "65x66x67_s4");
    testPanelEquiv(80, 240, 250, 4, threads, "80x240x250_s4");

    if (gFail) { std::cerr << "\nFAILED" << std::endl; return 1; }
    std::cout << "\nAll panel index tests passed" << std::endl;
    return 0;
}
