#include "erwt3d/writer.hpp"
#include "erwt3d/morton.hpp"
#include <fstream>
#include <vector>
#include <cstring>
#include <algorithm>
#include <iostream>

namespace erwt3d {

static void writeLeaves(std::ofstream& file, const ERWT3DHeader& header,
                        const std::vector<float>& superBuffer) {
    uint64_t leafBytes = getLeafBytes(header);
    std::vector<float> leafBuffer(header.leaf_x * header.leaf_y * header.leaf_z);
    uint64_t totalLeafs = getTotalLeafsPerSuper(header);
    uint64_t superY = header.super_y;
    for (uint64_t j = 0; j < totalLeafs; ++j) {
        uint32_t lx, ly, lz; unmorton3D(j, lx, ly, lz);
        if (lx >= getLeafsPerSuperX(header) || ly >= getLeafsPerSuperY(header) || lz >= getLeafsPerSuperZ(header)) continue;
        uint64_t bx = lx*header.leaf_x, by = ly*header.leaf_y, bz = lz*header.leaf_z;
        for (uint64_t z=0; z<header.leaf_z; ++z)
            for (uint64_t y=0; y<header.leaf_y; ++y)
                for (uint64_t x=0; x<header.leaf_x; ++x)
                    leafBuffer[(z*header.leaf_y+y)*header.leaf_x+x] =
                        superBuffer[((bz+z)*superY+(by+y))*header.super_x+(bx+x)];
        file.write(reinterpret_cast<const char*>(leafBuffer.data()), leafBytes);
    }
}

static void fillSuperBuffer(std::vector<float>& sb, const float* rawData,
                            uint64_t nx, uint64_t ny, uint64_t nz,
                            uint64_t sx, uint64_t sy, uint64_t sz,
                            uint64_t superX, uint64_t superY, uint64_t superZ) {
    std::memset(sb.data(), 0, superX*superY*superZ*sizeof(float));
    uint64_t startX = sx*superX, startY = sy*superY, startZ = sz*superZ;
    for (uint64_t z = 0; z < superZ; ++z) {
        uint64_t gz = startZ+z; if (gz >= nz) break;
        for (uint64_t y = 0; y < superY; ++y) {
            uint64_t gy = startY+y; if (gy >= ny) break;
            for (uint64_t x = 0; x < superX; ++x) {
                uint64_t gx = startX+x; if (gx >= nx) break;
                sb[(z*superY+y)*superX+x] = rawData[(gz*ny+gy)*nx+gx];
            }
        }
    }
}

static void fillSuperBufferFromFile(std::vector<float>& sb, std::ifstream& inFile,
                                     uint64_t nx, uint64_t ny, uint64_t nz,
                                     uint64_t sx, uint64_t sy, uint64_t sz,
                                     uint64_t superX, uint64_t superY, uint64_t superZ) {
    std::memset(sb.data(), 0, superX*superY*superZ*sizeof(float));
    uint64_t startX = sx*superX, startY = sy*superY, startZ = sz*superZ;
    for (uint64_t z = 0; z < superZ; ++z) {
        uint64_t gz = startZ+z; if (gz >= nz) break;
        for (uint64_t y = 0; y < superY; ++y) {
            uint64_t gy = startY+y; if (gy >= ny) break;
            uint64_t foff = ((gz*ny+gy)*nx+startX)*sizeof(float);
            uint64_t vx = std::min(static_cast<uint64_t>(superX), nx-startX);
            inFile.seekg(foff); inFile.clear();
            std::vector<float> row(vx);
            inFile.read(reinterpret_cast<char*>(row.data()), vx*sizeof(float));
            for (uint64_t x=0; x<vx; ++x) sb[(z*superY+y)*superX+x] = row[x];
        }
    }
}

bool writeERWT3D(const std::string& outputPath,
                 const float* rawData,
                 uint64_t nx, uint64_t ny, uint64_t nz,
                 uint32_t superX, uint32_t superY, uint32_t superZ,
                 uint32_t leafX, uint32_t leafY, uint32_t leafZ,
                 int, size_t,
                 uint32_t panelAxis, uint32_t panelStride) {
    bool doPanels = (panelAxis == 0) && panelStride > 0 && panelStride <= superX;

    ERWT3DHeader header;
    initHeader(header);
    header.nx=nx; header.ny=ny; header.nz=nz;
    header.super_x=superX; header.super_y=superY; header.super_z=superZ;
    header.leaf_x=leafX; header.leaf_y=leafY; header.leaf_z=leafZ;

    uint64_t sgX=getSuperGridX(header), sgY=getSuperGridY(header), sgZ=getSuperGridZ(header);
    uint64_t totalSB=sgX*sgY*sgZ, sbBytes=getSuperblockBytes(header);
    uint64_t rawSize=getRawSize(header);
    uint64_t planeBytes=superY*superZ*sizeof(float);
    uint64_t panelCount=doPanels?superX/panelStride:0;
    uint64_t sbPanelBytes=panelCount*planeBytes;
    uint64_t panelDataSize=totalSB*sbPanelBytes;
    uint64_t panelIndexBytes=totalSB*sizeof(uint64_t);
    uint64_t mainSize=sizeof(header)+totalSB*sbBytes;
    uint64_t projectedSize=mainSize+(doPanels?panelDataSize+panelIndexBytes:0);
    double projectedRatio=static_cast<double>(projectedSize)/rawSize;

    if (doPanels && projectedRatio > 1.45) {
        std::cerr << "Error: projected storage ratio " << projectedRatio
                  << "x exceeds 1.45x limit (main=" << mainSize << " panel=" << panelDataSize+panelIndexBytes << ")"
                  << std::endl;
        return false;
    }
    if (doPanels) {
        std::cout << "Projected storage ratio: " << projectedRatio
                  << "x (main=" << mainSize << " panel=" << panelDataSize+panelIndexBytes << ")" << std::endl;
    }

    std::ofstream file(outputPath, std::ios::binary);
    if (!file) return false;
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));

    std::vector<float> sb(superX*superY*superZ);

    // Pass 1: write superblocks (in-memory data, streaming)
    for (uint64_t sz=0; sz<sgZ; ++sz)
        for (uint64_t sy=0; sy<sgY; ++sy)
            for (uint64_t sx=0; sx<sgX; ++sx) {
                fillSuperBuffer(sb, rawData, nx, ny, nz, sx, sy, sz, superX, superY, superZ);
                writeLeaves(file, header, sb);
            }

    // Pass 2: generate panels from in-memory raw data
    if (doPanels) {
        uint64_t panelIndexOff=static_cast<uint64_t>(file.tellp());
        std::vector<uint64_t> panelIndex(totalSB);
        file.seekp(panelIndexOff+panelIndexBytes); // skip index for now
        std::vector<float> plane(superY*superZ);
        for (uint64_t sz=0; sz<sgZ; ++sz)
            for (uint64_t sy=0; sy<sgY; ++sy)
                for (uint64_t sx=0; sx<sgX; ++sx) {
                    uint64_t si=(sz*sgY+sy)*sgX+sx;
                    panelIndex[si]=static_cast<uint64_t>(file.tellp());
                    // Re-fill superbuffer (no snapshot!)
                    fillSuperBuffer(sb, rawData, nx, ny, nz, sx, sy, sz, superX, superY, superZ);
                    for (uint32_t lx=0; lx<superX; lx+=panelStride) {
                        for (uint64_t z=0; z<superZ; ++z)
                            for (uint64_t y=0; y<superY; ++y)
                                plane[z*superY+y]=sb[(z*superY+y)*superX+lx];
                        file.write(reinterpret_cast<const char*>(plane.data()), planeBytes);
                    }
                }
        uint64_t panelDataStart=panelIndexOff+panelIndexBytes;
        uint64_t panelEnd=static_cast<uint64_t>(file.tellp());
        file.seekp(panelIndexOff);
        file.write(reinterpret_cast<const char*>(panelIndex.data()), panelIndexBytes);
        file.seekp(0);
        header.flags|=FLAG_HAS_X_PANELS;
        header.reserved[0]=panelStride;
        header.reserved[3]=panelDataStart;
        header.reserved[4]=panelIndexOff;
        header.reserved[5]=panelEnd-panelDataStart;
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    }
    return true;
}

bool writeERWT3DFromFile(const std::string& outputPath,
                         const std::string& inputPath,
                         uint64_t nx, uint64_t ny, uint64_t nz,
                         uint32_t superX, uint32_t superY, uint32_t superZ,
                         uint32_t leafX, uint32_t leafY, uint32_t leafZ,
                         int, size_t,
                         uint32_t panelAxis, uint32_t panelStride) {
    bool doPanels = (panelAxis == 0) && panelStride > 0 && panelStride <= superX;

    ERWT3DHeader header;
    initHeader(header);
    header.nx=nx; header.ny=ny; header.nz=nz;
    header.super_x=superX; header.super_y=superY; header.super_z=superZ;
    header.leaf_x=leafX; header.leaf_y=leafY; header.leaf_z=leafZ;

    uint64_t sgX=getSuperGridX(header), sgY=getSuperGridY(header), sgZ=getSuperGridZ(header);
    uint64_t totalSB=sgX*sgY*sgZ, sbBytes=getSuperblockBytes(header);
    uint64_t rawSize=getRawSize(header);
    uint64_t planeBytes=superY*superZ*sizeof(float);
    uint64_t panelCount=doPanels?superX/panelStride:0;
    uint64_t sbPanelBytes=panelCount*planeBytes;
    uint64_t panelDataSize=totalSB*sbPanelBytes;
    uint64_t panelIndexBytes=totalSB*sizeof(uint64_t);
    uint64_t mainSize=sizeof(header)+totalSB*sbBytes;
    uint64_t projectedSize=mainSize+(doPanels?panelDataSize+panelIndexBytes:0);
    double projectedRatio=static_cast<double>(projectedSize)/rawSize;

    if (doPanels && projectedRatio > 1.45) {
        std::cerr << "Error: projected storage ratio " << projectedRatio
                  << "x exceeds 1.45x limit (main=" << mainSize << " panel=" << panelDataSize+panelIndexBytes << ")"
                  << std::endl;
        return false;
    }
    if (doPanels) {
        std::cout << "Projected storage ratio: " << projectedRatio
                  << "x (main=" << mainSize << " panel=" << panelDataSize+panelIndexBytes << ")" << std::endl;
    }

    // Pass 1: write header + superblocks
    {
        std::ifstream inFile(inputPath, std::ios::binary);
        if (!inFile) return false;
        std::ofstream outFile(outputPath, std::ios::binary);
        if (!outFile) return false;
        outFile.write(reinterpret_cast<const char*>(&header), sizeof(header));
        std::vector<float> sb(superX*superY*superZ);
        for (uint64_t sz=0; sz<sgZ; ++sz)
            for (uint64_t sy=0; sy<sgY; ++sy)
                for (uint64_t sx=0; sx<sgX; ++sx) {
                    fillSuperBufferFromFile(sb, inFile, nx, ny, nz, sx, sy, sz, superX, superY, superZ);
                    writeLeaves(outFile, header, sb);
                }
    } // close files

    // Pass 2: generate panels (streaming, one superblock at a time)
    if (doPanels) {
        std::ifstream inFile(inputPath, std::ios::binary);
        if (!inFile) return false;
        std::fstream outFile(outputPath, std::ios::binary | std::ios::in | std::ios::out);
        if (!outFile) return false;
        outFile.seekp(0, std::ios::end);
        uint64_t panelIndexOff=static_cast<uint64_t>(outFile.tellp());
        std::vector<uint64_t> panelIndex(totalSB);
        outFile.seekp(panelIndexOff+panelIndexBytes);
        std::vector<float> sb(superX*superY*superZ);
        std::vector<float> plane(superY*superZ);
        for (uint64_t sz=0; sz<sgZ; ++sz)
            for (uint64_t sy=0; sy<sgY; ++sy)
                for (uint64_t sx=0; sx<sgX; ++sx) {
                    uint64_t si=(sz*sgY+sy)*sgX+sx;
                    panelIndex[si]=static_cast<uint64_t>(outFile.tellp());
                    fillSuperBufferFromFile(sb, inFile, nx, ny, nz, sx, sy, sz, superX, superY, superZ);
                    for (uint32_t lx=0; lx<superX; lx+=panelStride) {
                        for (uint64_t z=0; z<superZ; ++z)
                            for (uint64_t y=0; y<superY; ++y)
                                plane[z*superY+y]=sb[(z*superY+y)*superX+lx];
                        outFile.write(reinterpret_cast<const char*>(plane.data()), planeBytes);
                    }
                }
        uint64_t panelDataStart=panelIndexOff+panelIndexBytes;
        uint64_t panelEnd=static_cast<uint64_t>(outFile.tellp());
        outFile.seekp(panelIndexOff);
        outFile.write(reinterpret_cast<const char*>(panelIndex.data()), panelIndexBytes);
        outFile.seekp(0);
        header.flags|=FLAG_HAS_X_PANELS;
        header.reserved[0]=panelStride;
        header.reserved[3]=panelDataStart;
        header.reserved[4]=panelIndexOff;
        header.reserved[5]=panelEnd-panelDataStart;
        outFile.write(reinterpret_cast<const char*>(&header), sizeof(header));
    }
    return true;
}

} // namespace erwt3d
