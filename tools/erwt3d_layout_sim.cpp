#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <iomanip>
#include <algorithm>
#include <string>

struct LayoutResult {
    std::string layout, axis;
    uint64_t blocks_touched, bytes_read, slice_bytes, contiguous_runs, metadata_bytes;
    double read_amplification, storage_ratio;
    uint64_t storage_total, panel_hit_bytes, panel_miss_bytes;
    double panel_hit_rate, panel_expected_bytes;
    // leaf-index merged extent estimates
    uint64_t leaf_count, merged_4KB, merged_16KB, merged_64KB, est_syscalls;
};

static uint64_t ceilDiv(uint64_t a, uint64_t b) { return (a+b-1)/b; }

void runSB(const char* label, uint64_t nx, uint64_t ny, uint64_t nz,
           uint32_t sb, uint32_t leaf, int px, int py,
           std::vector<LayoutResult>& out) {
    uint64_t sgX=ceilDiv(nx,sb), sgY=ceilDiv(ny,sb), sgZ=ceilDiv(nz,sb);
    uint64_t totalSB=sgX*sgY*sgZ, sbBytes=sb*sb*sb*4, leafBytes=leaf*leaf*leaf*4, rawSize=nx*ny*nz*4;
    uint64_t headerSz=256;

    uint64_t panelData=0, panelIdx=0;
    if (px>0) { uint64_t ppsb=sb/px; panelData+=totalSB*ppsb*(sb*sb*4); panelIdx+=totalSB*8; }
    if (py>0) { uint64_t ppsb=sb/py; panelData+=totalSB*ppsb*(sb*sb*4); panelIdx+=totalSB*8; }
    uint64_t totalStorage=headerSz+totalSB*sbBytes+panelData+panelIdx;

    const char* anames[3]={"x","y","z"};
    uint64_t sbsTouched[3]={sgY*sgZ, sgX*sgZ, sgX*sgY};
    uint64_t runs[3]={sgY*sgZ, sgZ, sgY};
    uint64_t sliceBytes[3]={ny*nz*4, nx*nz*4, nx*ny*4};

    for (int a=0;a<3;++a) {
        LayoutResult r;
        r.layout=label; r.axis=anames[a];
        r.blocks_touched=sbsTouched[a]; r.bytes_read=sbsTouched[a]*sbBytes;
        r.slice_bytes=sliceBytes[a]; r.contiguous_runs=runs[a];
        r.metadata_bytes=headerSz+panelIdx; r.storage_total=totalStorage;
        r.read_amplification=(double)r.bytes_read/r.slice_bytes;
        r.storage_ratio=(double)totalStorage/rawSize;
        r.leaf_count=r.merged_4KB=r.merged_16KB=r.merged_64KB=r.est_syscalls=0;

        // Panel-aware: expected bytes = hit_rate*panel_bytes + miss_rate*sb_bytes
        r.panel_hit_bytes=0; r.panel_miss_bytes=0; r.panel_hit_rate=0; r.panel_expected_bytes=0;
        if ((a==0&&px>0)||(a==1&&py>0)) {
            int ps=a==0?px:py;
            r.panel_hit_rate=1.0/ps;
            uint64_t panelBytes=sb*sb*4; // YZ or XZ plane
            r.panel_hit_bytes=panelBytes*sbsTouched[a];
            r.panel_miss_bytes=sbBytes*sbsTouched[a];
            r.panel_expected_bytes=r.panel_hit_rate*r.panel_hit_bytes+(1-r.panel_hit_rate)*r.panel_miss_bytes;
        }
        out.push_back(r);
    }
}

void runLeafIndex(const char* label, uint64_t nx, uint64_t ny, uint64_t nz,
                  uint32_t sb, uint32_t leaf, std::vector<LayoutResult>& out) {
    uint64_t sgX=ceilDiv(nx,sb), sgY=ceilDiv(ny,sb), sgZ=ceilDiv(nz,sb);
    uint64_t totalSB=sgX*sgY*sgZ, sbBytes=sb*sb*sb*4, leafBytes=leaf*leaf*leaf*4, rawSize=nx*ny*nz*4;
    uint64_t lpsbX=sb/leaf, lpsbY=sb/leaf, lpsbZ=sb/leaf;
    uint64_t leavesPerSB=lpsbX*lpsbY*lpsbZ;
    uint64_t indexPerSB=leavesPerSB*8, totalIndex=totalSB*indexPerSB;
    uint64_t totalStorage=256+totalSB*sbBytes+totalIndex;

    uint64_t xLeafs=sgY*sgZ*lpsbY*lpsbZ; // leaves touched for X-slice
    uint64_t yLeafs=sgX*sgZ*lpsbX*lpsbZ;
    uint64_t zLeafs=sgX*sgY*lpsbX*lpsbY;

    uint64_t leafCounts[3]={xLeafs,yLeafs,zLeafs};
    uint64_t sbsTouched[3]={sgY*sgZ,sgX*sgZ,sgX*sgY};
    uint64_t sliceBytes[3]={ny*nz*4,nx*nz*4,nx*ny*4};
    const char* anames[3]={"x","y","z"};

    for (int a=0;a<3;++a) {
        uint64_t lc=leafCounts[a];
        // Estimate merged extent counts at different page sizes
        // Leaves within a superblock along one dimension are contiguous in Morton order
        // Conservative estimate: leaves per superblock per slice = lpsbY*lpsbZ for X
        // Merged at 4KB: can merge up to 4KB/256B=16 leaves if contiguous
        uint64_t m4k=std::max(uint64_t(1),lc/16);   // ~1/16 of leaf count
        uint64_t m16k=std::max(uint64_t(1),lc/64);   // ~1/64
        uint64_t m64k=std::max(uint64_t(1),lc/256);  // ~1/256

        LayoutResult r;
        r.layout=label; r.axis=anames[a];
        r.blocks_touched=lc; r.bytes_read=lc*leafBytes;
        r.slice_bytes=sliceBytes[a]; r.contiguous_runs=m4k;  // best-case runs at 4KB
        r.metadata_bytes=totalIndex; r.storage_total=totalStorage;
        r.read_amplification=(double)r.bytes_read/r.slice_bytes;
        r.storage_ratio=(double)totalStorage/rawSize;
        r.leaf_count=lc; r.merged_4KB=m4k; r.merged_16KB=m16k; r.merged_64KB=m64k;
        r.est_syscalls=m16k; // 16KB merged as realistic estimate
        r.panel_hit_rate=0; r.panel_hit_bytes=0; r.panel_miss_bytes=0; r.panel_expected_bytes=0;
        out.push_back(r);
    }
}

void runMicroblock(const char* label, uint64_t nx, uint64_t ny, uint64_t nz,
                   uint32_t mb, std::vector<LayoutResult>& out) {
    uint64_t gX=ceilDiv(nx,mb), gY=ceilDiv(ny,mb), gZ=ceilDiv(nz,mb);
    uint64_t totalMB=gX*gY*gZ, mbBytes=mb*mb*mb*4, rawSize=nx*ny*nz*4;
    uint64_t indexBytes=totalMB*8, totalStorage=256+totalMB*mbBytes+indexBytes;

    uint64_t blocks[3]={gY*gZ,gX*gZ,gX*gY};
    uint64_t sliceBytes[3]={ny*nz*4,nx*nz*4,nx*ny*4};
    const char* anames[3]={"x","y","z"};

    for (int a=0;a<3;++a) {
        LayoutResult r;
        r.layout=label; r.axis=anames[a];
        r.blocks_touched=blocks[a]; r.bytes_read=blocks[a]*mbBytes;
        r.slice_bytes=sliceBytes[a]; r.contiguous_runs=blocks[a];
        r.metadata_bytes=indexBytes; r.storage_total=totalStorage;
        r.read_amplification=(double)r.bytes_read/r.slice_bytes;
        r.storage_ratio=(double)totalStorage/rawSize;
        r.leaf_count=r.merged_4KB=r.merged_16KB=r.merged_64KB=r.est_syscalls=0;
        r.panel_hit_rate=0; r.panel_hit_bytes=0; r.panel_miss_bytes=0; r.panel_expected_bytes=0;
        out.push_back(r);
    }
}

int main(int argc, char* argv[]) {
    uint64_t nx=0, ny=0, nz=0; std::string outPath, layout="all";
    uint32_t sb=64, leaf=4, tileX=0, tileY=0, tileZ=0, blockBytes=0;
    double panelBudget=0;

    for (int i=1;i<argc;++i) {
        if (!std::strcmp(argv[i],"--nx")&&i+1<argc) nx=std::stoull(argv[++i]);
        else if (!std::strcmp(argv[i],"--ny")&&i+1<argc) ny=std::stoull(argv[++i]);
        else if (!std::strcmp(argv[i],"--nz")&&i+1<argc) nz=std::stoull(argv[++i]);
        else if (!std::strcmp(argv[i],"--output")&&i+1<argc) outPath=argv[++i];
        else if (!std::strcmp(argv[i],"--layout")&&i+1<argc) layout=argv[++i];
        else if (!std::strcmp(argv[i],"--tile-x")&&i+1<argc) tileX=std::stoul(argv[++i]);
        else if (!std::strcmp(argv[i],"--tile-y")&&i+1<argc) tileY=std::stoul(argv[++i]);
        else if (!std::strcmp(argv[i],"--tile-z")&&i+1<argc) tileZ=std::stoul(argv[++i]);
        else if (!std::strcmp(argv[i],"--block-bytes")&&i+1<argc) blockBytes=std::stoul(argv[++i]);
        else if (!std::strcmp(argv[i],"--panel-budget-ratio")&&i+1<argc) panelBudget=std::stod(argv[++i]);
    }

    if (!nx||!ny||!nz) {
        std::cerr << "Usage: erwt3d_layout_sim --nx N --ny N --nz N [options]\n"
                  << "  --layout current-sb|microblock|leaf-index|xy-micro-panel|multiscale|all\n"
                  << "  --tile-x TX --tile-y TY --tile-z TZ (superblock size, default 64)\n"
                  << "  --block-bytes BYTES (microblock bytes)\n"
                  << "  --panel-budget-ratio R (max storage ratio for panels, default none)\n"
                  << "  --output layout_sim.csv\n"
                  << "Note: --random-count/--continuous-count/--seed deferred\n";
        return 1;
    }

    uint32_t ux=tileX?tileX:sb, uy=tileY?tileY:sb, uz=tileZ?tileZ:sb;
    std::vector<LayoutResult> results;

    auto run=[&](const std::string& name){
        if (layout!="all" && layout!=name) return;
        if (name=="current-sb"||name=="all"||layout=="current-sb")
            runSB("sb-default",nx,ny,nz,ux,leaf,0,0,results);
        if (name=="xy-micro-panel"||name=="all"||layout=="xy-micro-panel") {
            runSB("sb+xpanel-s4",nx,ny,nz,ux,leaf,4,0,results);
            runSB("sb+xypanel-s4",nx,ny,nz,ux,leaf,4,4,results);
        }
        if (name=="leaf-index"||name=="all"||layout=="leaf-index")
            runLeafIndex("leaf-index",nx,ny,nz,ux,leaf,results);
        if (name=="microblock"||name=="all"||layout=="microblock") {
            uint32_t mb=blockBytes>0?uint32_t(std::cbrt(blockBytes/4)):16;
            runMicroblock(("microblock-"+std::to_string(mb)).c_str(),nx,ny,nz,mb,results);
        }
        if (name=="multiscale"||(name=="all"&&layout!="microblock"&&layout!="leaf-index"&&layout!="xy-micro-panel")) {
            // Multiscale: defer or simple 2-level estimate
            // Not fully implemented; mark deferred
        }
    };

    run("all");

    std::ofstream f(outPath.empty()?"/dev/stdout":outPath);
    f << "layout,axis,blocks_touched,bytes_read,slice_bytes,read_amplification,contiguous_runs,metadata_bytes,storage_total,storage_ratio";
    f << ",leaf_count,merged_extent_4KB,merged_extent_16KB,merged_extent_64KB,est_syscalls";
    f << ",panel_hit_rate,panel_hit_bytes,panel_miss_bytes,panel_expected_bytes\n";
    for (auto& r : results) {
        f << r.layout<<","<<r.axis<<","<<r.blocks_touched<<","
          << r.bytes_read<<","<<r.slice_bytes<<","
          << std::fixed << std::setprecision(2) << r.read_amplification<<"x,"
          << r.contiguous_runs<<","<<r.metadata_bytes<<","
          << r.storage_total<<","<<r.storage_ratio<<","
          << r.leaf_count<<","<<r.merged_4KB<<","<<r.merged_16KB<<","<<r.merged_64KB<<","<<r.est_syscalls<<","
          << r.panel_hit_rate<<","<<r.panel_hit_bytes<<","<<r.panel_miss_bytes<<","<<r.panel_expected_bytes<<"\n";
    }
    if (!outPath.empty()) std::cout << "Layout simulation written to " << outPath << std::endl;
    return 0;
}
