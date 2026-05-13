#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <iomanip>
#include <algorithm>

struct LayoutMetrics {
    std::string layout;
    std::string axis;
    uint64_t blocks_touched;
    uint64_t bytes_read;
    uint64_t metadata_bytes;
    uint64_t storage_total;
    uint64_t contiguous_runs;
    double read_amplification;
    double storage_ratio;
    uint64_t slice_bytes;
};

void computeSB(const char* label, uint64_t nx, uint64_t ny, uint64_t nz,
               uint32_t sb, uint32_t leaf, uint32_t panelStrideX, uint32_t panelStrideY,
               std::vector<LayoutMetrics>& out) {
    uint64_t sgX = (nx+sb-1)/sb, sgY = (ny+sb-1)/sb, sgZ = (nz+sb-1)/sb;
    uint64_t totalSB = sgX*sgY*sgZ;
    uint64_t sbBytes = sb*sb*sb*4;
    uint64_t leafBytes = leaf*leaf*leaf*4;
    uint64_t rawSize = nx*ny*nz*4;

    // Per-axis metrics
    struct AxisInfo { uint64_t blocks, runs; };
    AxisInfo axes[3] = {
        {sgY*sgZ, sgY*sgZ},   // X: one sb per YZ cell, no contiguous runs
        {sgX*sgZ, sgZ},        // Y: sgX contiguous per Z row
        {sgX*sgY, sgY},        // Z: sgX contiguous per Y row
    };

    uint64_t panelStorage = 0;
    if (panelStrideX > 0) panelStorage += totalSB * (sb/panelStrideX) * (sb*sb*4);
    if (panelStrideY > 0) panelStorage += totalSB * (sb/panelStrideY) * (sb*sb*4);
    uint64_t panelIndex = (panelStrideX>0||panelStrideY>0) ? totalSB*8 : 0;
    uint64_t totalStorage = 256 + totalSB*sbBytes + panelStorage + panelIndex;

    const char* anames[3] = {"x","y","z"};
    uint64_t sliceBytes[3] = {ny*nz*4, nx*nz*4, nx*ny*4};

    for (int a=0;a<3;++a) {
        LayoutMetrics m;
        m.layout = label;
        m.axis = anames[a];
        m.blocks_touched = axes[a].blocks;
        m.bytes_read = axes[a].blocks * sbBytes;
        m.metadata_bytes = 256;
        m.storage_total = totalStorage;
        m.contiguous_runs = axes[a].runs;
        m.slice_bytes = sliceBytes[a];
        m.read_amplification = (double)m.bytes_read / m.slice_bytes;
        m.storage_ratio = (double)totalStorage / rawSize;
        out.push_back(m);
    }
}

void computeLeafIndex(const char* label, uint64_t nx, uint64_t ny, uint64_t nz,
                      uint32_t sb, uint32_t leaf,
                      std::vector<LayoutMetrics>& out) {
    uint64_t sgX=(nx+sb-1)/sb, sgY=(ny+sb-1)/sb, sgZ=(nz+sb-1)/sb;
    uint64_t totalSB = sgX*sgY*sgZ;
    uint64_t sbBytes = sb*sb*sb*4;
    uint64_t leafBytes = leaf*leaf*leaf*4;
    uint64_t leavesPerSb = (sb/leaf)*(sb/leaf)*(sb/leaf);
    uint64_t rawSize = nx*ny*nz*4;

    // Leaf index: for each superblock, an index mapping (leaf_x, leaf_y, leaf_z) -> file_offset
    // Index size per superblock = leavesPerSb * 8 bytes
    uint64_t indexPerSB = leavesPerSb * 8;
    uint64_t totalIndex = totalSB * indexPerSB;
    uint64_t totalStorage = 256 + totalSB*sbBytes + totalIndex;

    // With leaf index, we can read only needed leaf blocks per slice
    // For X-slice: need 1 leaf per YZ leaf cell = (ny/leaf)*(nz/leaf) per X-slice... actually
    // each superblock contributes leafs_per_superY * leafs_per_superZ leafs to an X-slice
    uint64_t leafsPerSuperY = sb/leaf, leafsPerSuperZ = sb/leaf, leafsPerSuperX = sb/leaf;
    uint64_t xLeafs = sgY*sgZ * leafsPerSuperY*leafsPerSuperZ; // touched leaf blocks for X
    uint64_t yLeafs = sgX*sgZ * leafsPerSuperX*leafsPerSuperZ;
    uint64_t zLeafs = sgX*sgY * leafsPerSuperX*leafsPerSuperY;

    uint64_t blocks[3] = {xLeafs, yLeafs, zLeafs};
    uint64_t sliceBytes[3] = {ny*nz*4, nx*nz*4, nx*ny*4};
    const char* anames[3] = {"x","y","z"};

    for (int a=0;a<3;++a) {
        LayoutMetrics m;
        m.layout = label;
        m.axis = anames[a];
        m.blocks_touched = blocks[a];
        m.bytes_read = blocks[a] * leafBytes;
        m.metadata_bytes = totalIndex;
        m.storage_total = totalStorage;
        m.contiguous_runs = blocks[a];
        m.slice_bytes = sliceBytes[a];
        m.read_amplification = (double)m.bytes_read / m.slice_bytes;
        m.storage_ratio = (double)totalStorage / rawSize;
        out.push_back(m);
    }
}

void computeMicroblock(const char* label, uint64_t nx, uint64_t ny, uint64_t nz,
                       uint32_t mb, std::vector<LayoutMetrics>& out) {
    uint64_t gX=(nx+mb-1)/mb, gY=(ny+mb-1)/mb, gZ=(nz+mb-1)/mb;
    uint64_t totalMB = gX*gY*gZ;
    uint64_t mbBytes = mb*mb*mb*4;
    uint64_t rawSize = nx*ny*nz*4;

    // Simple microblock storage: each microblock stored sequentially + index
    uint64_t indexBytes = totalMB * 8;
    uint64_t totalStorage = 256 + totalMB*mbBytes + indexBytes;

    uint64_t blocks[3] = {gY*gZ, gX*gZ, gX*gY};
    uint64_t sliceBytes[3] = {ny*nz*4, nx*nz*4, nx*ny*4};
    const char* anames[3] = {"x","y","z"};

    for (int a=0;a<3;++a) {
        LayoutMetrics m;
        m.layout = label;
        m.axis = anames[a];
        m.blocks_touched = blocks[a];
        m.bytes_read = blocks[a] * mbBytes;
        m.metadata_bytes = indexBytes;
        m.storage_total = totalStorage;
        m.contiguous_runs = blocks[a];
        m.slice_bytes = sliceBytes[a];
        m.read_amplification = (double)m.bytes_read / m.slice_bytes;
        m.storage_ratio = (double)totalStorage / rawSize;
        out.push_back(m);
    }
}

int main(int argc, char* argv[]) {
    uint64_t nx=0, ny=0, nz=0;
    std::string outPath;

    for (int i=1;i<argc;++i) {
        if (!std::strcmp(argv[i],"--nx")&&i+1<argc) nx=std::stoull(argv[++i]);
        else if (!std::strcmp(argv[i],"--ny")&&i+1<argc) ny=std::stoull(argv[++i]);
        else if (!std::strcmp(argv[i],"--nz")&&i+1<argc) nz=std::stoull(argv[++i]);
        else if (!std::strcmp(argv[i],"--output")&&i+1<argc) outPath=argv[++i];
    }

    if (!nx||!ny||!nz) { std::cerr<<"Usage: erwt3d_layout_sim --nx N --ny N --nz N --output layout_sim.csv\n"; return 1; }

    std::vector<LayoutMetrics> results;

    // Layout 1: Current SB (baseline)
    computeSB("sb-default", nx,ny,nz, 64,4, 0,0, results);

    // Layout 2: SB + X-panel s4 (current 20G best)
    computeSB("sb+xpanel-s4", nx,ny,nz, 64,4, 4,0, results);

    // Layout 3: SB + XY-panel s4
    computeSB("sb+xypanel-s4", nx,ny,nz, 64,4, 4,4, results);

    // Layout 4: Leaf-index
    computeLeafIndex("leaf-index", nx,ny,nz, 64,4, results);

    // Layout 5: Microblock 32³
    computeMicroblock("microblock-32", nx,ny,nz, 32, results);

    // Layout 6: Microblock 16³
    computeMicroblock("microblock-16", nx,ny,nz, 16, results);

    std::ofstream f(outPath.empty() ? "/dev/stdout" : outPath);
    f << "layout,axis,blocks_touched,bytes_read,slice_bytes,read_amplification,contiguous_runs,metadata_bytes,storage_total,storage_ratio\n";
    for (auto& m : results) {
        f << m.layout << "," << m.axis << "," << m.blocks_touched << ","
          << m.bytes_read << "," << m.slice_bytes << ","
          << std::fixed << std::setprecision(2) << m.read_amplification << "x,"
          << m.contiguous_runs << "," << m.metadata_bytes << ","
          << m.storage_total << ","
          << m.storage_ratio << "\n";
    }

    if (!outPath.empty()) std::cout << "Layout simulation written to " << outPath << std::endl;
    return 0;
}
