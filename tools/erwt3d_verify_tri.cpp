#include "erwt3d/tri_reader.hpp"
#include "erwt3d/tri_format.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <cmath>
#include <fcntl.h>
#include <unistd.h>
#include <random>
#include <algorithm>

void printUsage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " --raw PATH --tri PATH --nx N --ny N --nz N [options]\n"
        << "  --samples N         Number of random sample points (default: 100000)\n"
        << "  --rel-tol V         Relative error threshold (default: 1e-3)\n"
        << "  --zero-abs-tol V    Near-zero absolute threshold (default: 1e-6)\n"
        << "  --seed N            Random seed (default: 20260511)\n"
        << "  --threads N         Thread count for slice decoding (default: 1)\n"
        << "  --full              Check all points (not just samples)\n";
}

int main(int argc, char* argv[]) {
    std::string rawPath, triPath;
    uint64_t nx = 0, ny = 0, nz = 0;
    uint64_t numSamples = 100000;
    uint64_t seed = 20260511;
    double relTol = 1e-3;
    double zeroAbsTol = 1e-6;
    int numThreads = 1;
    bool fullCheck = false;

    auto next = [&](int& i) -> std::string {
        if (i + 1 >= argc) return "";
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--raw") rawPath = next(i);
        else if (arg == "--tri") triPath = next(i);
        else if (arg == "--nx") nx = std::stoull(next(i));
        else if (arg == "--ny") ny = std::stoull(next(i));
        else if (arg == "--nz") nz = std::stoull(next(i));
        else if (arg == "--samples") numSamples = std::stoull(next(i));
        else if (arg == "--rel-tol") relTol = std::stod(next(i));
        else if (arg == "--zero-abs-tol") zeroAbsTol = std::stod(next(i));
        else if (arg == "--seed") seed = std::stoull(next(i));
        else if (arg == "--threads") numThreads = std::stoi(next(i));
        else if (arg == "--full") fullCheck = true;
        else if (arg == "--help" || arg == "-h") { printUsage(argv[0]); return 0; }
        else { std::cerr << "Unknown arg: " << arg << "\n"; return 1; }
    }

    if (rawPath.empty() || triPath.empty() || nx == 0 || ny == 0 || nz == 0) {
        std::cerr << "Error: --raw, --tri, --nx, --ny, --nz are required\n";
        return 1;
    }

    erwt3d::TriReader reader(triPath);
    if (reader.getHeader().magic[0] == 0) {
        std::cerr << "Error: failed to open tri file\n";
        return 1;
    }
    if (numThreads > 1) reader.setNumThreads(numThreads);

    int fdRaw = open(rawPath.c_str(), O_RDONLY);
    if (fdRaw < 0) {
        std::cerr << "Error: cannot open raw file\n";
        return 1;
    }

    double maxRelError = 0;
    double maxAbsError = 0;
    uint64_t numFailed = 0;
    uint64_t numChecked = 0;

    auto checkPoint = [&](uint64_t x, uint64_t y, uint64_t z) {
        // Read raw value
        uint64_t rawOff = (z * ny * nx + y * nx + x) * sizeof(float);
        float rawVal;
        if (pread(fdRaw, &rawVal, sizeof(float), rawOff) != sizeof(float)) return;

        // Read tri value: read the appropriate slice
        // For efficiency, we'll read slices and check points within them
        // But for simplicity, just read the slice containing this point

        // This is slow but correct for verification
        erwt3d::TriSliceAxis axis;
        uint64_t index;
        uint64_t d1, d2; // within-slice coordinates

        // Choose the axis that gives the most coverage
        // For sampling, pick the axis randomly per point
        static std::mt19937 rng(seed);
        int ax = rng() % 3;

        if (ax == 0) { // X slice
            axis = erwt3d::TriSliceAxis::X;
            index = x;
            d1 = y; d2 = z;
        } else if (ax == 1) { // Y slice
            axis = erwt3d::TriSliceAxis::Y;
            index = y;
            d1 = x; d2 = z;
        } else { // Z slice
            axis = erwt3d::TriSliceAxis::Z;
            index = z;
            d1 = x; d2 = y;
        }

        // Read slice
        uint64_t outDim1, outDim2;
        if (ax == 0) { outDim1 = ny; outDim2 = nz; }
        else if (ax == 1) { outDim1 = nx; outDim2 = nz; }
        else { outDim1 = nx; outDim2 = ny; }

        std::vector<float> slice(outDim1 * outDim2);
        if (!reader.readSlice(axis, index, slice.data())) {
            std::cerr << "Error reading slice\n";
            return;
        }

        float triVal = slice[d2 * outDim1 + d1];

        double absErr = std::abs((double)rawVal - (double)triVal);
        double absRef = std::abs((double)rawVal);
        double relErr = absErr / std::max(absRef, 1e-12);

        bool failed;
        if (absRef <= zeroAbsTol) {
            failed = absErr > zeroAbsTol;
        } else {
            failed = relErr >= relTol;
        }

        if (absErr > maxAbsError) maxAbsError = absErr;
        if (relErr > maxRelError && absRef > zeroAbsTol) maxRelError = relErr;
        if (failed) numFailed++;
        numChecked++;
    };

    if (fullCheck) {
        // Check all points by reading each slice
        // For Z slices (most efficient for raw row-major)
        std::cerr << "Full verification: reading all Z slices...\n";
        for (uint64_t z = 0; z < nz; ++z) {
            std::vector<float> slice(nx * ny);
            if (!reader.readSlice(erwt3d::TriSliceAxis::Z, z, slice.data())) {
                std::cerr << "Error reading Z slice " << z << "\n";
                continue;
            }

            // Read raw z-layer
            std::vector<float> rawLayer(nx * ny);
            uint64_t rawOff = z * ny * nx * sizeof(float);
            uint64_t remaining = nx * ny * sizeof(float);
            char* p = reinterpret_cast<char*>(rawLayer.data());
            while (remaining > 0) {
                ssize_t rd = pread(fdRaw, p, remaining, rawOff);
                if (rd <= 0) break;
                remaining -= rd; rawOff += rd; p += rd;
            }

            for (uint64_t y = 0; y < ny; ++y) {
                for (uint64_t x = 0; x < nx; ++x) {
                    float rawVal = rawLayer[y * nx + x];
                    float triVal = slice[y * nx + x];

                    double absErr = std::abs((double)rawVal - (double)triVal);
                    double absRef = std::abs((double)rawVal);
                    double relErr = absErr / std::max(absRef, 1e-12);

                    bool failed;
                    if (absRef <= zeroAbsTol) {
                        failed = absErr > zeroAbsTol;
                    } else {
                        failed = relErr >= relTol;
                    }

                    if (absErr > maxAbsError) maxAbsError = absErr;
                    if (relErr > maxRelError && absRef > zeroAbsTol) maxRelError = relErr;
                    if (failed) numFailed++;
                    numChecked++;
                }
            }

            if (z % 100 == 0 || z == nz - 1) {
                std::cerr << "\r  z=" << z << "/" << nz << " checked=" << numChecked
                          << " failed=" << numFailed << std::flush;
            }
        }
        std::cerr << "\n";
    } else {
        std::cerr << "Sampling " << numSamples << " random points...\n";
        std::mt19937 rng(seed);
        std::uniform_int_distribution<uint64_t> distX(0, nx - 1);
        std::uniform_int_distribution<uint64_t> distY(0, ny - 1);
        std::uniform_int_distribution<uint64_t> distZ(0, nz - 1);

        // Group samples by slice for efficiency
        // Read one slice at a time, check all samples in that slice
        struct Sample {
            uint64_t x, y, z;
        };
        std::vector<Sample> samples;
        samples.reserve(numSamples);
        for (uint64_t i = 0; i < numSamples; ++i) {
            samples.push_back({distX(rng), distY(rng), distZ(rng)});
        }

        // Group by Z slice (most efficient for raw)
        std::sort(samples.begin(), samples.end(),
                  [](const Sample& a, const Sample& b) { return a.z < b.z; });

        uint64_t curZ = UINT64_MAX;
        std::vector<float> rawLayer, triSlice;
        for (const auto& s : samples) {
            if (s.z != curZ) {
                curZ = s.z;
                // Read raw z-layer
                rawLayer.resize(nx * ny);
                uint64_t rawOff = curZ * ny * nx * sizeof(float);
                uint64_t remaining = nx * ny * sizeof(float);
                char* p = reinterpret_cast<char*>(rawLayer.data());
                while (remaining > 0) {
                    ssize_t rd = pread(fdRaw, p, remaining, rawOff);
                    if (rd <= 0) break;
                    remaining -= rd; rawOff += rd; p += rd;
                }

                // Read tri z-slice
                triSlice.resize(nx * ny);
                reader.readSlice(erwt3d::TriSliceAxis::Z, curZ, triSlice.data());
            }

            float rawVal = rawLayer[s.y * nx + s.x];
            float triVal = triSlice[s.y * nx + s.x];

            double absErr = std::abs((double)rawVal - (double)triVal);
            double absRef = std::abs((double)rawVal);
            double relErr = absErr / std::max(absRef, 1e-12);

            bool failed;
            if (absRef <= zeroAbsTol) {
                failed = absErr > zeroAbsTol;
            } else {
                failed = relErr >= relTol;
            }

            if (absErr > maxAbsError) maxAbsError = absErr;
            if (relErr > maxRelError && absRef > zeroAbsTol) maxRelError = relErr;
            if (failed) numFailed++;
            numChecked++;
        }
    }

    close(fdRaw);

    std::cout << "Verification results:\n";
    std::cout << "  checked: " << numChecked << "\n";
    std::cout << "  failed: " << numFailed << "\n";
    std::cout << "  max_abs_error: " << maxAbsError << "\n";
    std::cout << "  max_rel_error: " << maxRelError << "\n";
    std::cout << "  rel_tol: " << relTol << "\n";
    std::cout << "  zero_abs_tol: " << zeroAbsTol << "\n";
    std::cout << "  passed: " << (numFailed == 0 ? "true" : "false") << "\n";

    return numFailed == 0 ? 0 : 1;
}
