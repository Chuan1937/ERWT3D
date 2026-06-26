#include "erwt3d/reader.hpp"
#include "erwt3d/morton.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <random>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <map>

#ifdef ERWT3D_HAVE_LZ4
#include <lz4.h>
#endif

void printUsage(const char* progName) {
    std::cerr << "Usage:" << std::endl;
    std::cerr << "  Compare raw file with ERWT3D:" << std::endl;
    std::cerr << "    " << progName << " --raw data.raw --erwt3d data.erwt3d --nx N --ny N --nz N [--samples N]" << std::endl;
    std::cerr << std::endl;
    std::cerr << "  Compare two raw files:" << std::endl;
    std::cerr << "    " << progName << " --raw-a data.raw --raw-b restored.raw --nx N --ny N --nz N" << std::endl;
}

int main(int argc, char* argv[]) {
    std::string rawPath;
    std::string erwt3dPath;
    std::string rawAPath, rawBPath;
    uint64_t nx = 0, ny = 0, nz = 0;
    uint64_t numSamples = 0;
    
    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--raw") == 0 && i + 1 < argc) {
            rawPath = argv[++i];
        } else if (std::strcmp(argv[i], "--erwt3d") == 0 && i + 1 < argc) {
            erwt3dPath = argv[++i];
        } else if (std::strcmp(argv[i], "--raw-a") == 0 && i + 1 < argc) {
            rawAPath = argv[++i];
        } else if (std::strcmp(argv[i], "--raw-b") == 0 && i + 1 < argc) {
            rawBPath = argv[++i];
        } else if (std::strcmp(argv[i], "--nx") == 0 && i + 1 < argc) {
            nx = std::stoull(argv[++i]);
        } else if (std::strcmp(argv[i], "--ny") == 0 && i + 1 < argc) {
            ny = std::stoull(argv[++i]);
        } else if (std::strcmp(argv[i], "--nz") == 0 && i + 1 < argc) {
            nz = std::stoull(argv[++i]);
        } else if (std::strcmp(argv[i], "--samples") == 0 && i + 1 < argc) {
            numSamples = std::stoull(argv[++i]);
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << argv[i] << std::endl;
            printUsage(argv[0]);
            return 1;
        }
    }
    
    if (nx == 0 || ny == 0 || nz == 0) {
        std::cerr << "Error: --nx, --ny, --nz are required" << std::endl;
        printUsage(argv[0]);
        return 1;
    }
    
    uint64_t totalElements = nx * ny * nz;
    
    if (!rawPath.empty() && !erwt3dPath.empty()) {
        // Compare raw file with ERWT3D
        std::cout << "Comparing raw file with ERWT3D..." << std::endl;
        
        double maxAbsError = 0.0;
        double maxRelError = 0.0;
        uint64_t numFailed = 0;
        
        if (numSamples > 0 && numSamples < totalElements) {
            // Streaming random sampling (does not load full files into RAM)
            std::cout << "Using streaming sampling mode (" << numSamples << " samples)" << std::endl;
            
            // Open raw file for pread
            int rawFd = open(rawPath.c_str(), O_RDONLY);
            if (rawFd < 0) {
                std::cerr << "Error: Cannot open raw file" << std::endl;
                return 1;
            }
            
            // Open ERWT3D file for superblock reads
            erwt3d::ERWT3DReader reader(erwt3dPath);
            const auto& hdr = reader.getHeader();
            uint64_t superBytes = erwt3d::getSuperblockBytes(hdr);
            uint64_t leafBytes = erwt3d::getLeafBytes(hdr);
            uint64_t gridX = erwt3d::getSuperGridX(hdr);
            uint64_t gridY = erwt3d::getSuperGridY(hdr);
            
            int erwt3dFd = open(erwt3dPath.c_str(), O_RDONLY);
            if (erwt3dFd < 0) {
                std::cerr << "Error: Cannot open ERWT3D file" << std::endl;
                close(rawFd);
                return 1;
            }
            
            // Generate sample indices and group by superblock
            srand(42);
            std::map<uint64_t, std::vector<std::pair<uint64_t, uint64_t>>> sbGroups;
            
            uint64_t nxy = static_cast<uint64_t>(nx) * ny;
            for (uint64_t s = 0; s < numSamples; ++s) {
                uint64_t idx = rand() % totalElements;
                
                // Convert linear index to (x, y, z)
                uint64_t x = idx % nx;
                uint64_t y = (idx / nx) % ny;
                uint64_t z = idx / nxy;
                
                // Compute superblock
                uint64_t sx = x / hdr.super_x;
                uint64_t sy_local = y / hdr.super_y;
                uint64_t sz = z / hdr.super_z;
                uint64_t sbIdx = (sz * gridY + sy_local) * gridX + sx;
                
                // Local coords within superblock
                uint64_t lx = x % hdr.super_x;
                uint64_t ly = y % hdr.super_y;
                uint64_t lz = z % hdr.super_z;
                
                // Leaf index within superblock
                uint64_t leafLx = lx / hdr.leaf_x;
                uint64_t leafLy = ly / hdr.leaf_y;
                uint64_t leafLz = lz / hdr.leaf_z;
                uint64_t leafMorton = erwt3d::morton3D(static_cast<uint32_t>(leafLx),
                                                        static_cast<uint32_t>(leafLy),
                                                        static_cast<uint32_t>(leafLz));
                
                // Element offset within leaf
                uint64_t inLeafX = lx % hdr.leaf_x;
                uint64_t inLeafY = ly % hdr.leaf_y;
                uint64_t inLeafZ = lz % hdr.leaf_z;
                uint64_t elemOff = (inLeafZ * hdr.leaf_y + inLeafY) * hdr.leaf_x + inLeafX;
                
                // byte offset within superblock
                uint64_t byteOffsetInSb = leafMorton * leafBytes + elemOff * sizeof(float);
                
                sbGroups[sbIdx].push_back({idx, byteOffsetInSb});
            }
            
            std::vector<uint8_t> sbBuf(superBytes);
            
            // Load compression index if needed
            bool compressed = erwt3d::isCompressed(hdr);
            std::vector<erwt3d::CompressedBlockIndex> compIdx;
            if (compressed) {
                uint64_t idxOff = erwt3d::getCompressionIndexOffset(hdr);
                uint64_t idxCnt = erwt3d::getCompressedBlockCount(hdr);
                compIdx.resize(idxCnt);
                pread(erwt3dFd, compIdx.data(), idxCnt * sizeof(erwt3d::CompressedBlockIndex), idxOff);
            }
            std::vector<uint8_t> compBuf(compressed ? superBytes : 0);
            
            for (const auto& [sbIdx, samples] : sbGroups) {
                // Read entire superblock from ERWT3D
                if (compressed && sbIdx < compIdx.size()) {
                    const auto& entry = compIdx[sbIdx];
                    if (entry.is_compressed) {
#ifdef ERWT3D_HAVE_LZ4
                        if (compBuf.size() < entry.compressed_size) compBuf.resize(entry.compressed_size);
                        if (pread(erwt3dFd, compBuf.data(), entry.compressed_size, entry.file_offset) != static_cast<ssize_t>(entry.compressed_size)) {
                            std::cerr << "Error: failed to read compressed block " << sbIdx << std::endl;
                            close(erwt3dFd); close(rawFd); return 1;
                        }
                        if (LZ4_decompress_safe(reinterpret_cast<const char*>(compBuf.data()), reinterpret_cast<char*>(sbBuf.data()), entry.compressed_size, superBytes) != static_cast<int>(superBytes)) {
                            std::cerr << "Error: failed to decompress block " << sbIdx << std::endl;
                            close(erwt3dFd); close(rawFd); return 1;
                        }
#else
                        std::cerr << "Error: lz4 not available" << std::endl;
                        close(erwt3dFd); close(rawFd); return 1;
#endif
                    } else {
                        if (pread(erwt3dFd, sbBuf.data(), superBytes, entry.file_offset) != static_cast<ssize_t>(superBytes)) {
                            std::cerr << "Error: failed to read block " << sbIdx << std::endl;
                            close(erwt3dFd); close(rawFd); return 1;
                        }
                    }
                } else {
                    uint64_t sbOff = hdr.data_offset + sbIdx * superBytes;
                    if (pread(erwt3dFd, sbBuf.data(), superBytes, sbOff) != static_cast<ssize_t>(superBytes)) {
                        std::cerr << "Error: failed to read superblock " << sbIdx << std::endl;
                        close(erwt3dFd);
                        close(rawFd);
                        return 1;
                    }
                }
                
                for (const auto& [linIdx, byteOffSb] : samples) {
                    // Read raw value
                    float rawVal;
                    if (pread(rawFd, &rawVal, sizeof(float), linIdx * sizeof(float)) != sizeof(float)) {
                        std::cerr << "Error: failed to read raw index " << linIdx << std::endl;
                        close(erwt3dFd);
                        close(rawFd);
                        return 1;
                    }
                    
                    // Extract ERWT3D value from superblock buffer
                    float erwt3dVal = *reinterpret_cast<const float*>(sbBuf.data() + byteOffSb);
                    
                    double diff = std::abs(rawVal - erwt3dVal);
                    double relDiff = diff / (std::abs(rawVal) + 1e-10);
                    
                    maxAbsError = std::max(maxAbsError, diff);
                    maxRelError = std::max(maxRelError, relDiff);
                    
                    if (diff > 1e-3) {
                        ++numFailed;
                    }
                }
            }
            
            close(erwt3dFd);
            close(rawFd);
        } else {
            // Full comparison (loads both files into RAM)
            std::ifstream rawFile(rawPath, std::ios::binary);
            if (!rawFile) {
                std::cerr << "Error: Cannot open raw file" << std::endl;
                return 1;
            }
            
            std::vector<float> rawData(totalElements);
            rawFile.read(reinterpret_cast<char*>(rawData.data()), totalElements * sizeof(float));
            if (!rawFile) {
                std::cerr << "Error: Failed to read raw file" << std::endl;
                return 1;
            }
            
            erwt3d::ERWT3DReader reader(erwt3dPath);
            std::vector<float> erwt3dData(totalElements);
            if (!reader.readFull(erwt3dData.data())) {
                std::cerr << "Error: Failed to read ERWT3D file" << std::endl;
                return 1;
            }
            
            for (uint64_t i = 0; i < totalElements; ++i) {
                double diff = std::abs(rawData[i] - erwt3dData[i]);
                double relDiff = diff / (std::abs(rawData[i]) + 1e-10);
                
                maxAbsError = std::max(maxAbsError, diff);
                maxRelError = std::max(maxRelError, relDiff);
                
                if (diff > 1e-3) {
                    ++numFailed;
                }
            }
        }
        
        // Report
        std::cout << "max_abs_error: " << maxAbsError << std::endl;
        std::cout << "max_rel_error: " << maxRelError << std::endl;
        std::cout << "num_failed: " << numFailed << std::endl;
        std::cout << "passed: " << (numFailed == 0 ? "true" : "false") << std::endl;
        
        return numFailed == 0 ? 0 : 1;
        
    } else if (!rawAPath.empty() && !rawBPath.empty()) {
        // Compare two raw files
        std::cout << "Comparing two raw files..." << std::endl;
        
        std::ifstream rawAFile(rawAPath, std::ios::binary);
        std::ifstream rawBFile(rawBPath, std::ios::binary);
        
        if (!rawAFile || !rawBFile) {
            std::cerr << "Error: Cannot open raw files" << std::endl;
            return 1;
        }
        
        std::vector<float> rawA(totalElements);
        std::vector<float> rawB(totalElements);
        
        rawAFile.read(reinterpret_cast<char*>(rawA.data()), totalElements * sizeof(float));
        rawBFile.read(reinterpret_cast<char*>(rawB.data()), totalElements * sizeof(float));
        
        if (!rawAFile || !rawBFile) {
            std::cerr << "Error: Failed to read raw files" << std::endl;
            return 1;
        }
        
        // Compare
        double maxAbsError = 0.0;
        double maxRelError = 0.0;
        uint64_t numFailed = 0;
        
        for (uint64_t i = 0; i < totalElements; ++i) {
            double diff = std::abs(rawA[i] - rawB[i]);
            double relDiff = diff / (std::abs(rawA[i]) + 1e-10);
            
            maxAbsError = std::max(maxAbsError, diff);
            maxRelError = std::max(maxRelError, relDiff);
            
            if (diff > 1e-3) {
                ++numFailed;
            }
        }
        
        // Report
        std::cout << "max_abs_error: " << maxAbsError << std::endl;
        std::cout << "max_rel_error: " << maxRelError << std::endl;
        std::cout << "num_failed: " << numFailed << std::endl;
        std::cout << "passed: " << (numFailed == 0 ? "true" : "false") << std::endl;
        
        return numFailed == 0 ? 0 : 1;
        
    } else {
        std::cerr << "Error: Must specify either --raw/--erwt3d or --raw-a/--raw-b" << std::endl;
        printUsage(argv[0]);
        return 1;
    }
}