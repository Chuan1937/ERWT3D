#include "erwt3d/writer.hpp"
#include "erwt3d/morton.hpp"
#include <fstream>
#include <vector>
#include <cstring>
#include <algorithm>

namespace erwt3d {

bool writeERWT3D(const std::string& outputPath,
                 const float* rawData,
                 uint64_t nx, uint64_t ny, uint64_t nz,
                 uint32_t superX, uint32_t superY, uint32_t superZ,
                 uint32_t leafX, uint32_t leafY, uint32_t leafZ,
                 int numThreads, size_t memoryLimitMB,
                 uint32_t panelAxis, uint32_t panelStride) {
    ERWT3DHeader header;
    initHeader(header);
    header.nx = nx; header.ny = ny; header.nz = nz;
    header.super_x = superX; header.super_y = superY; header.super_z = superZ;
    header.leaf_x = leafX; header.leaf_y = leafY; header.leaf_z = leafZ;

    uint64_t superGridX = getSuperGridX(header);
    uint64_t superGridY = getSuperGridY(header);
    uint64_t superGridZ = getSuperGridZ(header);
    uint64_t superBytes = getSuperblockBytes(header);
    uint64_t leafBytes = getLeafBytes(header);

    std::ofstream file(outputPath, std::ios::binary);
    if (!file) return false;
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));

    std::vector<float> superBuffer(superX * superY * superZ);
    std::vector<float> leafBuffer(leafX * leafY * leafZ);

    for (uint64_t sz = 0; sz < superGridZ; ++sz) {
        for (uint64_t sy = 0; sy < superGridY; ++sy) {
            for (uint64_t sx = 0; sx < superGridX; ++sx) {
                std::memset(superBuffer.data(), 0, superBytes);
                uint64_t startX = sx * superX, startY = sy * superY, startZ = sz * superZ;
                for (uint64_t z = 0; z < superZ; ++z) {
                    uint64_t gz = startZ + z; if (gz >= nz) break;
                    for (uint64_t y = 0; y < superY; ++y) {
                        uint64_t gy = startY + y; if (gy >= ny) break;
                        for (uint64_t x = 0; x < superX; ++x) {
                            uint64_t gx = startX + x; if (gx >= nx) break;
                            superBuffer[(z*superY+y)*superX+x] = rawData[(gz*ny+gy)*nx+gx];
                        }
                    }
                }

                uint64_t totalLeafs = getTotalLeafsPerSuper(header);
                for (uint64_t j = 0; j < totalLeafs; ++j) {
                    uint32_t lx, ly, lz; unmorton3D(j, lx, ly, lz);
                    if (lx >= getLeafsPerSuperX(header) || ly >= getLeafsPerSuperY(header) || lz >= getLeafsPerSuperZ(header)) continue;
                    uint64_t bx = lx*leafX, by = ly*leafY, bz = lz*leafZ;
                    for (uint64_t z=0; z<leafZ; ++z) for (uint64_t y=0; y<leafY; ++y) for (uint64_t x=0; x<leafX; ++x)
                        leafBuffer[(z*leafY+y)*leafX+x] = superBuffer[((bz+z)*superY+(by+y))*superX+(bx+x)];
                    file.write(reinterpret_cast<const char*>(leafBuffer.data()), leafBytes);
                }
            }
        }
    }

    // ---- Panel generation ----
    if (panelStride > 0 && (panelAxis == 0 || panelAxis == 1 || panelAxis == 2) && panelStride <= superX) {
        // We need superblock snapshots but they were already written.
        // For the in-memory path, re-read raw data to generate panels.
        uint64_t totalSB = superGridX * superGridY * superGridZ;
        std::vector<std::vector<float>> sbSnapshots(totalSB);
        for (uint64_t sz = 0; sz < superGridZ; ++sz) {
            for (uint64_t sy = 0; sy < superGridY; ++sy) {
                for (uint64_t sx = 0; sx < superGridX; ++sx) {
                    uint64_t sbIdx = (sz * superGridY + sy) * superGridX + sx;
                    auto& sb = sbSnapshots[sbIdx];
                    sb.resize(superX * superY * superZ, 0.0f);
                    uint64_t startX = sx * superX, startY = sy * superY, startZ = sz * superZ;
                    for (uint64_t z = 0; z < superZ; ++z) {
                        uint64_t gz = startZ + z; if (gz >= nz) break;
                        for (uint64_t y = 0; y < superY; ++y) {
                            uint64_t gy = startY + y; if (gy >= ny) break;
                            for (uint64_t x = 0; x < superX; ++x) {
                                uint64_t gx = startX + x; if (gx >= nx) break;
                                sb[(z*superY+y)*superX+x] = rawData[(gz*ny+gy)*nx+gx];
                            }
                        }
                    }
                }
            }
        }

        uint64_t panelIndexOffset = static_cast<uint64_t>(file.tellp());
        uint64_t panelCount = superX / panelStride;
        uint64_t planeBytes = panelPlaneBytes(header);
        uint64_t sbPanelBytes = panelCount * planeBytes;
        uint64_t panelDataStart = panelIndexOffset + totalSB * sizeof(uint64_t);
        std::vector<uint64_t> panelIndex(totalSB, panelDataStart);
        file.write(reinterpret_cast<const char*>(panelIndex.data()), totalSB * sizeof(uint64_t));

        std::vector<float> plane(superY * superZ);
        for (uint64_t si = 0; si < totalSB; ++si) {
            panelIndex[si] = static_cast<uint64_t>(file.tellp());
            const auto& sb = sbSnapshots[si];
            for (uint32_t lx = 0; lx < superX; lx += panelStride) {
                for (uint64_t z = 0; z < superZ; ++z)
                    for (uint64_t y = 0; y < superY; ++y)
                        plane[z*superY+y] = sb[(z*superY+y)*superX+lx];
                file.write(reinterpret_cast<const char*>(plane.data()), planeBytes);
            }
        }
        uint64_t panelEnd = static_cast<uint64_t>(file.tellp());

        file.seekp(panelIndexOffset);
        file.write(reinterpret_cast<const char*>(panelIndex.data()), totalSB * sizeof(uint64_t));

        file.seekp(0);
        header.flags |= (panelAxis == 0 ? FLAG_HAS_X_PANELS :
                         panelAxis == 1 ? FLAG_HAS_Y_PANELS :
                         FLAG_HAS_Z_PANELS);
        header.reserved[0] = panelStride;
        header.reserved[3] = panelDataStart;
        header.reserved[4] = panelIndexOffset;
        header.reserved[5] = panelEnd - panelDataStart;
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    }
    (void)numThreads; (void)memoryLimitMB;
    return true;
}

bool writeERWT3DFromFile(const std::string& outputPath,
                         const std::string& inputPath,
                         uint64_t nx, uint64_t ny, uint64_t nz,
                         uint32_t superX, uint32_t superY, uint32_t superZ,
                         uint32_t leafX, uint32_t leafY, uint32_t leafZ,
                         int numThreads, size_t memoryLimitMB,
                         uint32_t panelAxis, uint32_t panelStride) {
    ERWT3DHeader header;
    initHeader(header);
    header.nx = nx; header.ny = ny; header.nz = nz;
    header.super_x = superX; header.super_y = superY; header.super_z = superZ;
    header.leaf_x = leafX; header.leaf_y = leafY; header.leaf_z = leafZ;

    uint64_t superGridX = getSuperGridX(header);
    uint64_t superGridY = getSuperGridY(header);
    uint64_t superGridZ = getSuperGridZ(header);
    uint64_t superBytes = getSuperblockBytes(header);
    uint64_t leafBytes = getLeafBytes(header);
    uint64_t totalSB = superGridX * superGridY * superGridZ;
    uint32_t stride = panelStride > 0 ? panelStride : 1;
    bool doPanels = (panelAxis == 0 || panelAxis == 1 || panelAxis == 2) && panelStride > 0 && panelStride <= superX;

    std::ifstream inFile(inputPath, std::ios::binary);
    if (!inFile) return false;

    std::ofstream outFile(outputPath, std::ios::binary);
    if (!outFile) return false;
    outFile.write(reinterpret_cast<const char*>(&header), sizeof(header));

    std::vector<float> superBuffer(superX * superY * superZ);
    std::vector<float> leafBuffer(leafX * leafY * leafZ);

    // For panels: store superblocks so we can generate panels after writing
    std::vector<std::vector<float>> sbSnapshots;
    if (doPanels) sbSnapshots.resize(totalSB);

    for (uint64_t sz = 0; sz < superGridZ; ++sz) {
        for (uint64_t sy = 0; sy < superGridY; ++sy) {
            for (uint64_t sx = 0; sx < superGridX; ++sx) {
                uint64_t sbIdx = (sz * superGridY + sy) * superGridX + sx;
                std::memset(superBuffer.data(), 0, superBytes);
                uint64_t startX = sx * superX, startY = sy * superY, startZ = sz * superZ;

                for (uint64_t z = 0; z < superZ; ++z) {
                    uint64_t gz = startZ + z; if (gz >= nz) break;
                    for (uint64_t y = 0; y < superY; ++y) {
                        uint64_t gy = startY + y; if (gy >= ny) break;
                        uint64_t foff = ((gz*ny + gy)*nx + startX) * sizeof(float);
                        uint64_t vx = std::min(static_cast<uint64_t>(superX), nx - startX);
                        inFile.seekg(foff); inFile.clear();
                        std::vector<float> row(vx);
                        inFile.read(reinterpret_cast<char*>(row.data()), vx*sizeof(float));
                        for (uint64_t x=0; x<vx; ++x) superBuffer[(z*superY+y)*superX+x] = row[x];
                    }
                }

                if (doPanels) sbSnapshots[sbIdx] = superBuffer;

                uint64_t totalLeafs = getTotalLeafsPerSuper(header);
                for (uint64_t j = 0; j < totalLeafs; ++j) {
                    uint32_t lx, ly, lz; unmorton3D(j, lx, ly, lz);
                    if (lx >= getLeafsPerSuperX(header) || ly >= getLeafsPerSuperY(header) || lz >= getLeafsPerSuperZ(header)) continue;
                    uint64_t bx = lx*leafX, by = ly*leafY, bz = lz*leafZ;
                    for (uint64_t z=0; z<leafZ; ++z) for (uint64_t y=0; y<leafY; ++y) for (uint64_t x=0; x<leafX; ++x)
                        leafBuffer[(z*leafY+y)*leafX+x] = superBuffer[((bz+z)*superY+(by+y))*superX+(bx+x)];
                    outFile.write(reinterpret_cast<const char*>(leafBuffer.data()), leafBytes);
                }
            }
        }
    }

    // ---- Panel generation ----
    if (doPanels && !sbSnapshots.empty()) {
        uint64_t panelIndexOffset = static_cast<uint64_t>(outFile.tellp());
        uint64_t panelCount = 64 / stride;
        uint64_t planeBytes = panelPlaneBytes(header);
        uint64_t sbPanelBytes = panelCount * planeBytes;

        // Write panel index (one uint64_t per superblock)
        std::vector<uint64_t> panelIndex(totalSB, 0);
        // Placeholder: we'll fill after writing data
        uint64_t panelDataStart = panelIndexOffset + totalSB * sizeof(uint64_t);
        for (auto& pi : panelIndex) pi = panelDataStart;
        outFile.write(reinterpret_cast<const char*>(panelIndex.data()), totalSB * sizeof(uint64_t));

        // Write panel data
        std::vector<float> plane(superY * superZ);
        uint64_t actualOffset = static_cast<uint64_t>(outFile.tellp());
        // Fix panel index to point to actual data start
        for (auto& pi : panelIndex) pi = actualOffset;

        for (uint64_t si = 0; si < totalSB; ++si) {
            panelIndex[si] = static_cast<uint64_t>(outFile.tellp());
            const auto& sb = sbSnapshots[si];
            for (uint32_t localX = 0; localX < superX; localX += stride) {
                // Extract YZ plane at localX: Z-major order
                for (uint64_t z = 0; z < superZ; ++z)
                    for (uint64_t y = 0; y < superY; ++y)
                        plane[z*superY + y] = sb[(z*superY + y)*superX + localX];
                outFile.write(reinterpret_cast<const char*>(plane.data()), planeBytes);
            }
        }
        uint64_t panelEnd = static_cast<uint64_t>(outFile.tellp());

        // Go back and write panel index
        outFile.seekp(panelIndexOffset);
        outFile.write(reinterpret_cast<const char*>(panelIndex.data()), totalSB * sizeof(uint64_t));

        // Update header and rewrite at file start
        outFile.seekp(0);
        header.flags |= (panelAxis == 0 ? FLAG_HAS_X_PANELS :
                         panelAxis == 1 ? FLAG_HAS_Y_PANELS :
                         FLAG_HAS_Z_PANELS);
        header.reserved[0] = stride; // panel stride for this axis
        header.reserved[3] = panelDataStart;
        header.reserved[4] = panelIndexOffset;
        header.reserved[5] = panelEnd - panelDataStart;
        outFile.write(reinterpret_cast<const char*>(&header), sizeof(header));
    }

    (void)numThreads; (void)memoryLimitMB;
    return true;
}

} // namespace erwt3d
